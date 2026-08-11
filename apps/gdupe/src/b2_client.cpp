#include "b2_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace gdupe {
namespace {

struct CurlGlobal {
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("libcurl initialization failed");
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal curl_global;

std::string hex_digest(const unsigned char *digest, std::size_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i)
    output << std::setw(2) << static_cast<unsigned int>(digest[i]);
  return output.str();
}

std::string sha1_of(const std::string &value) {
  std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
  SHA1(reinterpret_cast<const unsigned char *>(value.data()), value.size(),
       digest.data());
  return hex_digest(digest.data(), digest.size());
}

std::string sha256_of(const std::string &value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(),
         digest.data());
  return hex_digest(digest.data(), digest.size());
}

std::string url_encode(const std::string &value,
                       bool preserve_slashes = false) {
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
  if (preserve_slashes) {
    std::size_t position = 0;
    while ((position = result.find("%2F", position)) != std::string::npos)
      result.replace(position, 3, "/");
  }
  return result;
}

std::string extension_of(const std::string &key) {
  const auto slash = key.find_last_of('/');
  const auto dot = key.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  std::string extension = key.substr(dot + 1);
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

bool same_inventory(const std::vector<RemoteObject> &a,
                    const std::vector<RemoteObject> &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].key != b[i].key || a[i].file_id != b[i].file_id ||
        a[i].size != b[i].size || a[i].sha1 != b[i].sha1)
      return false;
  }
  return true;
}

} // namespace

B2Client::B2Client(const Config &config) : config_(config) {
  authorize();
  ensure_bucket_id();
}

B2Client::Response B2Client::request(const std::string &method,
                                     const std::string &url,
                                     const std::vector<std::string> &headers,
                                     const std::string &body,
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
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "gdupe/1.0");
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
  } else if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return {status, static_cast<int>(result), std::move(response_body)};
}

const B2Client::Authorization &B2Client::authorize(bool force) {
  std::scoped_lock lock(authorization_mutex_);
  if (!authorization_.token.empty() && !force)
    return authorization_;
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    const auto response = request(
        "GET", "https://api.backblazeb2.com/b2api/v2/b2_authorize_account", {},
        {}, config_.key_id + ":" + config_.application_key);
    if (response.curl_code == CURLE_OK && response.status >= 200 &&
        response.status < 300) {
      const auto value = nlohmann::json::parse(response.body);
      authorization_ = {};
      authorization_.token = value.at("authorizationToken").get<std::string>();
      authorization_.api_url = value.at("apiUrl").get<std::string>();
      authorization_.download_url = value.at("downloadUrl").get<std::string>();
      authorization_.account_id = value.at("accountId").get<std::string>();
      const auto allowed = value.value("allowed", nlohmann::json::object());
      authorization_.bucket_id = allowed.value("bucketId", std::string{});
      for (const auto &capability :
           allowed.value("capabilities", nlohmann::json::array())) {
        authorization_.capabilities.insert(capability.get<std::string>());
      }
      const std::string restricted = allowed.value("bucketName", std::string{});
      if (!restricted.empty() && restricted != config_.bucket_name)
        throw std::runtime_error(
            "B2 credentials are restricted to another bucket");
      upload_url_.clear();
      upload_token_.clear();
      return authorization_;
    }
    if (!retryable(response.status, response.curl_code) ||
        attempt == config_.maximum_attempts) {
      throw std::runtime_error(
          "B2 authorization failed (HTTP " + std::to_string(response.status) +
          ", code=" + json_error_code(response.body) + ")");
    }
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(30, 1 << (attempt - 1))));
  }
  throw std::runtime_error("B2 authorization exhausted its retry budget");
}

void B2Client::require_capabilities(
    std::initializer_list<const char *> capabilities) {
  const auto &auth = authorize();
  for (const char *capability : capabilities) {
    if (!auth.capabilities.contains(capability))
      throw std::runtime_error(std::string("B2 credentials lack capability: ") +
                               capability);
  }
}

nlohmann::json B2Client::api(const std::string &method,
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
    if (response.status == 401 &&
        (code == "expired_auth_token" || code == "bad_auth_token"))
      authorize(true);
    if (!retryable(response.status, response.curl_code) ||
        attempt == config_.maximum_attempts) {
      throw std::runtime_error(operation + " failed (HTTP " +
                               std::to_string(response.status) +
                               ", code=" + code + ")");
    }
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
  }
  throw std::runtime_error(operation + " exhausted its retry budget");
}

void B2Client::ensure_bucket_id() {
  if (!authorization_.bucket_id.empty())
    return;
  require_capabilities({"listBuckets"});
  const auto value =
      api("b2_list_buckets", {{"accountId", authorization_.account_id}},
          "B2 bucket discovery");
  for (const auto &bucket : value.at("buckets")) {
    if (bucket.value("bucketName", std::string{}) == config_.bucket_name) {
      authorization_.bucket_id = bucket.at("bucketId").get<std::string>();
      return;
    }
  }
  throw std::runtime_error("Configured B2 bucket was not found");
}

std::vector<RemoteObject> B2Client::list_objects(const std::string &prefix) {
  require_capabilities({"listFiles"});
  ensure_bucket_id();
  std::vector<RemoteObject> result;
  std::string start;
  do {
    nlohmann::json body = {{"bucketId", authorization_.bucket_id},
                           {"prefix", prefix},
                           {"maxFileCount", 1000}};
    if (!start.empty())
      body["startFileName"] = start;
    const auto page = api("b2_list_file_names", body, "B2 inventory");
    for (const auto &value : page.value("files", nlohmann::json::array())) {
      if (value.value("action", std::string{}) != "upload" ||
          value.value("contentLength", 0LL) <= 0)
        continue;
      const auto info = value.value("fileInfo", nlohmann::json::object());
      std::string sha1 = value.value("contentSha1", std::string{});
      if (sha1 == "none")
        sha1 = info.value("large_file_sha1", std::string{});
      const std::string key = value.at("fileName").get<std::string>();
      result.push_back(
          {key, value.value("fileId", std::string{}),
           static_cast<std::uint64_t>(
               value.at("contentLength").get<std::int64_t>()),
           sha1,
           value.value("contentType", std::string("application/octet-stream")),
           extension_of(key)});
    }
    start = page.value("nextFileName", std::string{});
  } while (!start.empty());
  std::sort(
      result.begin(), result.end(),
      [](const auto &left, const auto &right) { return left.key < right.key; });
  return result;
}

std::vector<RemoteObject>
B2Client::stable_inventory(const std::string &prefix) {
  auto previous = list_objects(prefix);
  for (int pass = 0; pass < 4; ++pass) {
    auto current = list_objects(prefix);
    if (same_inventory(previous, current))
      return current;
    previous = std::move(current);
  }
  throw std::runtime_error("B2 inventory kept changing; destructive work is "
                           "locked until a stable snapshot is available");
}

std::optional<RemoteObject> B2Client::find_object(const std::string &key) {
  require_capabilities({"listFiles"});
  ensure_bucket_id();
  const auto value = api("b2_list_file_names",
                         {{"bucketId", authorization_.bucket_id},
                          {"startFileName", key},
                          {"maxFileCount", 1}},
                         "B2 object lookup");
  for (const auto &row : value.value("files", nlohmann::json::array())) {
    if (row.value("action", std::string{}) == "upload" &&
        row.value("fileName", std::string{}) == key &&
        row.value("contentLength", 0LL) > 0) {
      const auto info = row.value("fileInfo", nlohmann::json::object());
      std::string sha1 = row.value("contentSha1", std::string{});
      if (sha1 == "none")
        sha1 = info.value("large_file_sha1", std::string{});
      return RemoteObject{
          key,
          row.value("fileId", std::string{}),
          static_cast<std::uint64_t>(
              row.at("contentLength").get<std::int64_t>()),
          sha1,
          row.value("contentType", std::string("application/octet-stream")),
          extension_of(key)};
    }
  }
  return std::nullopt;
}

bool B2Client::version_exists(const std::string &key,
                              const std::string &file_id) {
  require_capabilities({"readFiles"});
  try {
    const auto value =
        api("b2_get_file_info", {{"fileId", file_id}}, "B2 version lookup");
    return value.value("fileId", std::string{}) == file_id &&
           value.value("fileName", std::string{}) == key;
  } catch (const std::runtime_error &problem) {
    const std::string message = problem.what();
    if (message.find("HTTP 404") != std::string::npos ||
        message.find("code=file_not_present") != std::string::npos)
      return false;
    throw;
  }
}

void B2Client::download_to(const RemoteObject &object,
                           const std::filesystem::path &destination) {
  require_capabilities({"readFiles"});
  std::filesystem::create_directories(destination.parent_path());
  const auto partial = destination.string() + ".partial";
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    if (!stream)
      throw std::runtime_error("Cannot create local media staging file");
    CURL *curl = curl_easy_init();
    if (!curl)
      throw std::runtime_error("Cannot allocate B2 download");
    curl_slist *headers = nullptr;
    headers = curl_slist_append(
        headers, ("Authorization: " + authorize().token).c_str());
    const std::string url =
        authorization_.download_url +
        "/b2api/v2/b2_download_file_by_id?fileId=" + url_encode(object.file_id);
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
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    stream.close();
    bool valid = code == CURLE_OK && status >= 200 && status < 300 &&
                 std::filesystem::file_size(partial) == object.size;
    if (valid) {
      std::ifstream input(partial, std::ios::binary);
      SHA_CTX context{};
      SHA1_Init(&context);
      std::array<char, 8 * 1024 * 1024> buffer{};
      while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0)
          SHA1_Update(&context, buffer.data(), static_cast<std::size_t>(count));
      }
      std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
      SHA1_Final(digest.data(), &context);
      valid = object.sha1.size() == 40 &&
              hex_digest(digest.data(), digest.size()) == object.sha1;
    }
    if (valid) {
      std::filesystem::remove(destination);
      std::filesystem::rename(partial, destination);
      return;
    }
    std::filesystem::remove(partial);
    if ((!retryable(status, static_cast<int>(code))) ||
        attempt == config_.maximum_attempts)
      throw std::runtime_error(
          "B2 media download or integrity verification failed for " +
          object.key);
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
  }
}

std::string B2Client::download_bytes(const RemoteObject &object) {
  const auto temporary = std::filesystem::temp_directory_path() /
                         ("gdupe-b2-" + sha1_of(object.file_id));
  download_to(object, temporary);
  std::ifstream stream(temporary, std::ios::binary);
  std::ostringstream body;
  body << stream.rdbuf();
  std::filesystem::remove(temporary);
  return body.str();
}

void B2Client::delete_version(const std::string &key,
                              const std::string &file_id) {
  require_capabilities({"deleteFiles"});
  const auto result =
      api("b2_delete_file_version", {{"fileName", key}, {"fileId", file_id}},
          "B2 exact-version deletion");
  if (result.value("fileName", std::string{}) != key ||
      result.value("fileId", std::string{}) != file_id)
    throw std::runtime_error("B2 deletion acknowledgement did not match the "
                             "intended object version");
}

void B2Client::ensure_upload_endpoint() {
  if (!upload_url_.empty())
    return;
  require_capabilities({"writeFiles"});
  ensure_bucket_id();
  const auto value =
      api("b2_get_upload_url", {{"bucketId", authorization_.bucket_id}},
          "B2 upload endpoint");
  upload_url_ = value.at("uploadUrl").get<std::string>();
  upload_token_ = value.at("authorizationToken").get<std::string>();
}

RemoteObject
B2Client::upload_bytes(const std::string &key, const std::string &payload,
                       const std::string &content_type,
                       const std::map<std::string, std::string> &metadata) {
  const std::string digest = sha1_of(payload);
  for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
    ensure_upload_endpoint();
    std::vector<std::string> headers = {
        "Authorization: " + upload_token_,
        "X-Bz-File-Name: " + url_encode(key, true),
        "Content-Type: " + content_type, "X-Bz-Content-Sha1: " + digest};
    for (const auto &[name, value] : metadata)
      headers.push_back("X-Bz-Info-" + name + ": " + url_encode(value));
    const auto response = request("POST", upload_url_, headers, payload);
    if (response.curl_code == CURLE_OK && response.status >= 200 &&
        response.status < 300) {
      const auto value = nlohmann::json::parse(response.body);
      if (value.value("fileName", std::string{}) != key ||
          value.value("contentSha1", std::string{}) != digest ||
          value.value("contentLength", -1LL) !=
              static_cast<std::int64_t>(payload.size())) {
        throw std::runtime_error("B2 upload acknowledgement did not match the "
                                 "canonical index payload");
      }
      return {key,
              value.value("fileId", std::string{}),
              static_cast<std::uint64_t>(payload.size()),
              digest,
              content_type,
              extension_of(key)};
    }
    upload_url_.clear();
    upload_token_.clear();
    if (!retryable(response.status, response.curl_code) ||
        attempt == config_.maximum_attempts)
      throw std::runtime_error("B2 canonical-index upload failed");
    std::this_thread::sleep_for(
        std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
  }
  throw std::runtime_error(
      "B2 canonical-index upload exhausted its retry budget");
}

std::string
B2Client::inventory_digest(const std::vector<RemoteObject> &objects) const {
  std::string canonical;
  for (const auto &item : objects) {
    canonical.append(item.key).push_back('\0');
    canonical.append(item.file_id).push_back('\0');
    canonical.append(std::to_string(item.size)).push_back('\0');
    canonical.append(item.sha1).push_back('\n');
  }
  return sha256_of(canonical);
}

std::string B2Client::canonical_index_payload(
    const std::vector<RemoteObject> &objects) const {
  nlohmann::json items = nlohmann::json::array();
  for (const auto &item : objects)
    items.push_back({{"key", item.key},
                     {"file_id", item.file_id},
                     {"size", item.size},
                     {"sha1", item.sha1},
                     {"content_type", item.content_type},
                     {"ext", item.extension}});
  nlohmann::json root = {{"version", 1},
                         {"inventory_sha256", inventory_digest(objects)},
                         {"count", objects.size()},
                         {"items", std::move(items)}};
  return root.dump(2) + "\n";
}

std::vector<RemoteObject>
B2Client::settle_canonical_index(const std::vector<RemoteObject> &initial) {
  auto inventory = initial;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::string payload = canonical_index_payload(inventory);
    const auto existing = find_object(config_.canonical_index_key);
    if (!existing || download_bytes(*existing) != payload) {
      const auto uploaded =
          upload_bytes(config_.canonical_index_key, payload, "application/json",
                       {{"gdupe-purpose", "canonical-inventory"},
                        {"gdupe-sha256", sha256_of(payload)}});
      if (download_bytes(uploaded) != payload)
        throw std::runtime_error("Canonical B2 index body verification failed");
    }
    const auto after = stable_inventory(config_.canonical_prefix);
    if (same_inventory(inventory, after))
      return after;
    inventory = after;
  }
  throw std::runtime_error("B2 inventory changed throughout canonical-index "
                           "publication; destructive work remains locked");
}

} // namespace gdupe
