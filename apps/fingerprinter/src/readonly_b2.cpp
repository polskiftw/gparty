#include "readonly_b2.hpp"

#include "crypto_hash.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace gparty::fingerprints {
namespace {

struct CurlGlobal {
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("libcurl initialization failed");
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal curl_global;
constexpr std::size_t kMaximumListingPages = 100'000;

std::string url_encode(const std::string &value) {
  CURL *curl = curl_easy_init();
  if (!curl)
    throw std::runtime_error("Cannot allocate URL encoder");
  char *encoded =
      curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  if (!encoded) {
    curl_easy_cleanup(curl);
    throw std::runtime_error("URL encoding failed");
  }
  std::string result(encoded);
  curl_free(encoded);
  curl_easy_cleanup(curl);
  return result;
}

std::string extension_of(const std::string &key) {
  const auto slash = key.find_last_of('/');
  const auto dot = key.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  std::string extension = key.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return extension;
}

bool retryable(long status, int curl_code) {
  return curl_code != CURLE_OK || status == 408 || status == 429 ||
         status == 500 || status == 502 || status == 503 || status == 504;
}

std::string json_error_code(const std::string &body) {
  try {
    const auto value = nlohmann::json::parse(body);
    return value.is_object() ? value.value("code", std::string{})
                             : std::string{};
  } catch (...) {
    return {};
  }
}

gdupe::RemoteObject object_from_json(const nlohmann::json &value) {
  const auto info = value.value("fileInfo", nlohmann::json::object());
  std::string sha1 = value.value("contentSha1", std::string{});
  if (sha1 == "none")
    sha1 = info.value("large_file_sha1", std::string{});
  const std::string key = value.at("fileName").get<std::string>();
  return {key,
          value.value("fileId", std::string{}),
          static_cast<std::uint64_t>(
              value.at("contentLength").get<std::int64_t>()),
          sha1,
          value.value("contentType", std::string("application/octet-stream")),
          extension_of(key),
          value.value("uploadTimestamp", 0LL)};
}

} // namespace

ReadOnlyB2Client::ReadOnlyB2Client(const Config &config) : config_(config) {
  authorize();
  ensure_bucket_id();
  require_capabilities({"listFiles", "readFiles"});
}

ReadOnlyB2Client::Response ReadOnlyB2Client::request(
    const std::string &method, const std::string &url,
    const std::vector<std::string> &headers, const std::string &body,
    const std::string &user_password) {
  CURL *curl = curl_easy_init();
  if (!curl)
    throw std::runtime_error("Cannot allocate an HTTP request");
  curl_slist *list = nullptr;
  for (const auto &header : headers)
    list = curl_slist_append(list, header.c_str());
  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "gparty-fingerprinter/1.0");
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      +[](char *data, std::size_t size, std::size_t count,
          void *target) -> std::size_t {
        static_cast<std::string *>(target)->append(data, size * count);
        return size * count;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  if (!user_password.empty()) {
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, user_password.c_str());
  }
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
  }
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return {status, static_cast<int>(result), std::move(response_body)};
}

const ReadOnlyB2Client::Authorization &
ReadOnlyB2Client::authorize(bool force) {
  std::scoped_lock lock(authorization_mutex_);
  if (!authorization_.token.empty() && !force)
    return authorization_;
  const std::string known_account_id = authorization_.account_id;
  const std::string known_bucket_id = authorization_.bucket_id;
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    const auto response = request(
        "GET", "https://api.backblazeb2.com/b2api/v2/b2_authorize_account",
        {}, {}, config_.key_id + ":" + config_.application_key);
    if (response.curl_code == CURLE_OK && response.status >= 200 &&
        response.status < 300) {
      const auto value = nlohmann::json::parse(response.body);
      authorization_ = {};
      authorization_.token = value.at("authorizationToken").get<std::string>();
      authorization_.api_url = value.at("apiUrl").get<std::string>();
      authorization_.download_url = value.at("downloadUrl").get<std::string>();
      authorization_.account_id = value.at("accountId").get<std::string>();
      if (!known_account_id.empty() &&
          authorization_.account_id != known_account_id)
        throw std::runtime_error("B2 account identity changed unexpectedly");
      const auto allowed = value.value("allowed", nlohmann::json::object());
      authorization_.bucket_id = allowed.value("bucketId", std::string{});
      if (authorization_.bucket_id.empty())
        authorization_.bucket_id = known_bucket_id;
      for (const auto &capability :
           allowed.value("capabilities", nlohmann::json::array()))
        authorization_.capabilities.insert(capability.get<std::string>());
      const std::string bucket = allowed.value("bucketName", std::string{});
      if (!bucket.empty() && bucket != config_.bucket_name)
        throw std::runtime_error(
            "B2 credentials are restricted to another bucket");
      return authorization_;
    }
    if (!retryable(response.status, response.curl_code) ||
        attempt == config_.maximum_attempts)
      throw std::runtime_error(
          "B2 authorization failed (HTTP " +
          std::to_string(response.status) + ", code=" +
          json_error_code(response.body) + ")");
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(30, 1 << (attempt - 1))));
  }
  throw std::runtime_error("B2 authorization exhausted its retry budget");
}

void ReadOnlyB2Client::require_capabilities(
    std::initializer_list<const char *> capabilities) {
  const auto &auth = authorize();
  for (const char *capability : capabilities)
    if (!auth.capabilities.contains(capability))
      throw std::runtime_error(std::string("B2 credentials lack capability: ") +
                               capability);
}

nlohmann::json ReadOnlyB2Client::api(const std::string &method,
                                     const nlohmann::json &body,
                                     const std::string &operation) {
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    const auto &auth = authorize();
    const auto response = request(
        "POST", auth.api_url + "/b2api/v2/" + method,
        {"Authorization: " + auth.token, "Content-Type: application/json"},
        body.dump());
    if (response.curl_code == CURLE_OK && response.status >= 200 &&
        response.status < 300)
      return nlohmann::json::parse(response.body);
    const std::string code = json_error_code(response.body);
    const bool expired =
        response.status == 401 &&
        (code == "expired_auth_token" || code == "bad_auth_token");
    if (expired)
      authorize(true);
    if ((!retryable(response.status, response.curl_code) && !expired) ||
        attempt == config_.maximum_attempts)
      throw std::runtime_error(operation + " failed (HTTP " +
                               std::to_string(response.status) +
                               ", code=" + code + ")");
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
  }
  throw std::runtime_error(operation + " exhausted its retry budget");
}

void ReadOnlyB2Client::ensure_bucket_id() {
  if (!authorization_.bucket_id.empty())
    return;
  require_capabilities({"listBuckets"});
  const auto value =
      api("b2_list_buckets", {{"accountId", authorization_.account_id}},
          "B2 bucket discovery");
  for (const auto &bucket : value.at("buckets"))
    if (bucket.value("bucketName", std::string{}) == config_.bucket_name) {
      authorization_.bucket_id = bucket.at("bucketId").get<std::string>();
      return;
    }
  throw std::runtime_error("Configured B2 bucket was not found");
}

std::vector<gdupe::RemoteObject>
ReadOnlyB2Client::list_objects(const std::string &prefix) {
  require_capabilities({"listFiles"});
  ensure_bucket_id();
  std::vector<gdupe::RemoteObject> result;
  std::string start;
  std::unordered_set<std::string> cursors;
  std::size_t pages = 0;
  do {
    if (++pages > kMaximumListingPages)
      throw std::runtime_error("B2 inventory exceeded its pagination limit");
    nlohmann::json body = {{"bucketId", authorization_.bucket_id},
                           {"prefix", prefix},
                           {"maxFileCount", 1000}};
    if (!start.empty())
      body["startFileName"] = start;
    const auto page = api("b2_list_file_names", body, "B2 inventory");
    for (const auto &value : page.value("files", nlohmann::json::array()))
      if (value.value("action", std::string{}) == "upload" &&
          value.value("contentLength", 0LL) > 0)
        result.push_back(object_from_json(value));
    const std::string next = page.value("nextFileName", std::string{});
    if (!next.empty() && !cursors.insert(next).second)
      throw std::runtime_error("B2 returned a repeated inventory cursor");
    start = next;
  } while (!start.empty());
  std::sort(result.begin(), result.end(), [](const auto &left,
                                             const auto &right) {
    return left.key < right.key;
  });
  return result;
}

std::optional<gdupe::RemoteObject>
ReadOnlyB2Client::find_object(const std::string &key) {
  require_capabilities({"listFiles"});
  const auto value = api("b2_list_file_names",
                         {{"bucketId", authorization_.bucket_id},
                          {"startFileName", key},
                          {"maxFileCount", 1}},
                         "B2 object lookup");
  for (const auto &row : value.value("files", nlohmann::json::array()))
    if (row.value("action", std::string{}) == "upload" &&
        row.value("fileName", std::string{}) == key &&
        row.value("contentLength", 0LL) > 0)
      return object_from_json(row);
  return std::nullopt;
}

void ReadOnlyB2Client::download_to(
    const gdupe::RemoteObject &object,
    const std::filesystem::path &destination, DownloadProgress progress) {
  require_capabilities({"readFiles"});
  std::filesystem::create_directories(destination.parent_path());
  const bool has_sha1 = object.sha1.size() == 40;
  if (std::filesystem::is_regular_file(destination) &&
      std::filesystem::file_size(destination) == object.size &&
      (!has_sha1 || gdupe::sha1_file(destination) == object.sha1))
    return;
  const auto partial = destination.string() + ".partial";
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream)
      throw std::runtime_error("Cannot create media staging file");
    CURL *curl = curl_easy_init();
    if (!curl)
      throw std::runtime_error("Cannot allocate B2 download");
    curl_slist *headers = nullptr;
    headers = curl_slist_append(
        headers, ("Authorization: " + authorize().token).c_str());
    const std::string url =
        authorization_.download_url +
        "/b2api/v2/b2_download_file_by_id?fileId=" +
        url_encode(object.file_id);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char *data, std::size_t size, std::size_t count,
            void *target) -> std::size_t {
          auto *output = static_cast<std::ofstream *>(target);
          output->write(data, static_cast<std::streamsize>(size * count));
          return output->good() ? size * count : 0;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
    if (progress) {
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(
          curl, CURLOPT_XFERINFOFUNCTION,
          +[](void *target, curl_off_t total, curl_off_t downloaded,
              curl_off_t, curl_off_t) -> int {
            auto *callback = static_cast<DownloadProgress *>(target);
            try {
              const bool keep_going = (*callback)(
                  static_cast<std::uint64_t>(
                      (std::max)(downloaded, curl_off_t{})),
                  static_cast<std::uint64_t>((std::max)(total, curl_off_t{})));
              return keep_going ? 0 : 1;
            } catch (...) {
              return 1;
            }
          });
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    }
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    stream.close();
    if (status == 401 && attempt < config_.maximum_attempts)
      authorize(true);
    if (code == CURLE_ABORTED_BY_CALLBACK) {
      std::filesystem::remove(partial);
      throw std::runtime_error("B2 download was cancelled");
    }
    const bool response_ok =
        code == CURLE_OK && status >= 200 && status < 300;
    bool valid = response_ok && std::filesystem::is_regular_file(partial) &&
                 std::filesystem::file_size(partial) == object.size;
    if (valid && has_sha1)
      valid = gdupe::sha1_file(partial) == object.sha1;
    if (valid) {
      std::filesystem::remove(destination);
      std::filesystem::rename(partial, destination);
      return;
    }
    std::filesystem::remove(partial);
    if ((!retryable(status, static_cast<int>(code)) && status != 401 &&
         !response_ok) ||
        attempt == config_.maximum_attempts)
      throw std::runtime_error(
          "B2 download or integrity verification failed for " + object.key);
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
  }
}

} // namespace gparty::fingerprints
