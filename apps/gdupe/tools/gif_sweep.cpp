#include "b2_client.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "database.hpp"
#include "fingerprint.hpp"
#include "wic_gif.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

constexpr wchar_t kGdupeInstanceMutex[] = L"Local\\gdupe-single-instance-v1";
constexpr const char *kBrokenDirectory = "broken-originals";
constexpr const char *kNormalizedDirectory = "normalized-originals";
constexpr std::size_t kPrefetchDepth = 8;

struct Options {
  std::optional<std::filesystem::path> config;
  std::optional<std::filesystem::path> report;
};

class GdupeProcessLock final {
public:
  GdupeProcessLock() {
    handle_ = CreateMutexW(nullptr, FALSE, kGdupeInstanceMutex);
    if (!handle_)
      throw std::runtime_error("Windows could not create the gdupe instance lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error(
          "Close gdupe before running the GIF sweep; the sweep writes clean "
          "fingerprints into gdupe's local database");
    }
  }

  ~GdupeProcessLock() {
    if (handle_)
      CloseHandle(handle_);
  }

  GdupeProcessLock(const GdupeProcessLock &) = delete;
  GdupeProcessLock &operator=(const GdupeProcessLock &) = delete;

private:
  HANDLE handle_{};
};

std::filesystem::path executable_path() {
  std::vector<wchar_t> buffer(1024);
  while (true) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0)
      throw std::runtime_error("Windows could not locate gdupe-gif-sweep.exe");
    if (length < buffer.size() - 1)
      return std::filesystem::path(std::wstring(buffer.data(), length));
    if (buffer.size() >= 32768)
      throw std::runtime_error("The GIF sweep executable path is too long");
    buffer.resize(buffer.size() * 2);
  }
}

Options parse_options(int argc, wchar_t **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--config" || argument == L"--report") {
      if (index + 1 >= argc)
        throw std::runtime_error("A command-line option is missing its path");
      const std::filesystem::path value(argv[++index]);
      if (argument == L"--config")
        options.config = value;
      else
        options.report = value;
      continue;
    }
    if (argument == L"--help" || argument == L"-h") {
      std::cout << "gdupe-gif-sweep [--config PATH] [--report PATH]\n";
      std::exit(0);
    }
    throw std::runtime_error("Unknown command-line option");
  }
  return options;
}

nlohmann::json normalization_json(
    const gdupe::WicGifNormalization &normalization) {
  return {{"frame", normalization.frame_index},
          {"left", normalization.left},
          {"top", normalization.top},
          {"declared_width", normalization.declared_width},
          {"declared_height", normalization.declared_height},
          {"decoded_width", normalization.decoded_width},
          {"decoded_height", normalization.decoded_height},
          {"expanded_canvas", normalization.expanded_canvas},
          {"size_metadata_mismatch",
           normalization.size_metadata_mismatch}};
}

nlohmann::json base_record(const gdupe::RemoteObject &object) {
  return {{"type", "gif"},
          {"key", object.key},
          {"file_id", object.file_id},
          {"size", object.size}};
}

void append_info(nlohmann::json &record, const gdupe::WicGifInfo &info) {
  record["logical_width"] = info.logical_width;
  record["logical_height"] = info.logical_height;
  record["effective_width"] = info.width;
  record["effective_height"] = info.height;
  record["frame_count"] = info.frame_count;
  record["normalizations"] = nlohmann::json::array();
  for (const auto &normalization : info.normalizations)
    record["normalizations"].push_back(normalization_json(normalization));
}

void write_record(std::ofstream &report, const nlohmann::json &record) {
  report << record.dump() << '\n';
  report.flush();
  if (!report)
    throw std::runtime_error("Could not write the GIF sweep report");
}

void remove_tool_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error)
    throw std::runtime_error("Could not remove a GIF sweep staging file: " +
                             error.message());
}

std::filesystem::path stable_gif_name(const gdupe::RemoteObject &object) {
  return gdupe::sha256(object.file_id).substr(0, 32) + ".gif";
}

bool exact_local_copy(const std::filesystem::path &path,
                      const gdupe::RemoteObject &object) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return false;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size != object.size)
    return false;
  return object.sha1.size() == 40 && gdupe::sha1_file(path) == object.sha1;
}

void require_finding_integrity(const std::filesystem::path &path,
                               const gdupe::RemoteObject &object) {
  if (!exact_local_copy(path, object)) {
    throw std::runtime_error(
        "A saved GIF finding no longer matches its B2 object: " +
        path.string());
  }
}

std::optional<std::filesystem::path>
existing_finding(const std::filesystem::path &findings_root,
                 const gdupe::RemoteObject &object) {
  for (const char *directory : {kNormalizedDirectory, kBrokenDirectory}) {
    const auto candidate = findings_root / directory / stable_gif_name(object);
    if (std::filesystem::exists(candidate)) {
      require_finding_integrity(candidate, object);
      return candidate;
    }
  }
  return std::nullopt;
}

std::filesystem::path
preserve_finding(const std::filesystem::path &source,
                 const std::filesystem::path &findings_root,
                 const char *directory, const gdupe::RemoteObject &object,
                 bool remove_source) {
  const auto destination =
      findings_root / directory / stable_gif_name(object);
  if (source == destination) {
    require_finding_integrity(destination, object);
    return destination;
  }
  if (std::filesystem::exists(destination)) {
    require_finding_integrity(destination, object);
    if (remove_source)
      remove_tool_file(source);
    return destination;
  }

  std::filesystem::create_directories(destination.parent_path());
  auto partial = destination;
  partial += ".partial";
  std::error_code cleanup_error;
  std::filesystem::remove(partial, cleanup_error);
  std::filesystem::copy_file(source, partial);
  if (!exact_local_copy(partial, object)) {
    std::filesystem::remove(partial, cleanup_error);
    throw std::runtime_error(
        "A GIF finding failed integrity verification while being preserved");
  }
  std::filesystem::rename(partial, destination);
  if (remove_source)
    remove_tool_file(source);
  return destination;
}

bool has_current_fingerprint(const gdupe::InventoryObject &item,
                             int fingerprint_version) {
  return item.fingerprint && item.fingerprint->version == fingerprint_version;
}

void verify_saved_fingerprint(const gdupe::Database &database,
                              const gdupe::RemoteObject &object,
                              const gdupe::Fingerprint &fingerprint) {
  const auto saved = database.object(object.key);
  if (!saved || saved->remote.file_id != object.file_id ||
      !saved->fingerprint ||
      saved->fingerprint->version != fingerprint.version ||
      saved->fingerprint->sha256 != fingerprint.sha256) {
    throw std::runtime_error(
        "Clean GIF fingerprint did not verify after the local database write");
  }
}

void print_activity_burst() {
  std::size_t width = 80;
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (output != INVALID_HANDLE_VALUE && output != nullptr &&
      GetConsoleScreenBufferInfo(output, &info)) {
    const SHORT columns =
        static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
    if (columns > 0)
      width = static_cast<std::size_t>(columns);
  }

  const std::string line(width, '#');
  for (int row = 0; row < 5; ++row)
    std::cout << line << '\n';
  std::cout.flush();
}

bool retryable_download(long status, CURLcode code) {
  return code != CURLE_OK || status == 408 || status == 429 || status == 500 ||
         status == 502 || status == 503 || status == 504;
}

std::size_t append_to_string(char *data, std::size_t size, std::size_t count,
                             void *target) {
  static_cast<std::string *>(target)->append(data, size * count);
  return size * count;
}

std::size_t write_to_file(char *data, std::size_t size, std::size_t count,
                          void *target) {
  auto *output = static_cast<std::ofstream *>(target);
  output->write(data, static_cast<std::streamsize>(size * count));
  return output->good() ? size * count : 0;
}

class CurlGlobalGuard final {
public:
  CurlGlobalGuard() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("libcurl initialization failed for GIF sweep");
  }

  ~CurlGlobalGuard() { curl_global_cleanup(); }

  CurlGlobalGuard(const CurlGlobalGuard &) = delete;
  CurlGlobalGuard &operator=(const CurlGlobalGuard &) = delete;
};

class SweepDownloader final {
public:
  explicit SweepDownloader(const gdupe::Config &config) : config_(config) {
    curl_ = curl_easy_init();
    if (!curl_)
      throw std::runtime_error("Cannot allocate persistent GIF sweep download");
    authorize();
  }

  ~SweepDownloader() {
    if (curl_)
      curl_easy_cleanup(curl_);
  }

  SweepDownloader(const SweepDownloader &) = delete;
  SweepDownloader &operator=(const SweepDownloader &) = delete;

  void download_to(const gdupe::RemoteObject &object,
                   const std::filesystem::path &destination) {
    std::filesystem::create_directories(destination.parent_path());
    if (exact_local_copy(destination, object))
      return;

    const auto partial = destination.string() + ".partial";
    for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
      std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
      if (!stream)
        throw std::runtime_error("Cannot create GIF sweep staging file");

      curl_slist *headers = nullptr;
      headers = curl_slist_append(
          headers, ("Authorization: " + authorization_token_).c_str());
      if (!headers)
        throw std::runtime_error("Cannot allocate GIF sweep HTTP headers");

      const std::string url =
          download_url_ + "/b2api/v2/b2_download_file_by_id?fileId=" +
          escape(object.file_id);
      configure_download(url, headers, stream);
      const CURLcode code = curl_easy_perform(curl_);
      long status = 0;
      curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
      curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
      curl_slist_free_all(headers);
      stream.close();

      if (status == 401 && attempt < config_.maximum_attempts)
        authorize();

      const bool successful_response =
          code == CURLE_OK && status >= 200 && status < 300;
      bool valid = successful_response;
      std::error_code size_error;
      if (valid) {
        valid = std::filesystem::file_size(partial, size_error) == object.size &&
                !size_error;
      }
      if (valid)
        valid = object.sha1.size() == 40 &&
                gdupe::sha1_file(partial) == object.sha1;
      if (valid) {
        std::filesystem::remove(destination);
        std::filesystem::rename(partial, destination);
        return;
      }

      std::error_code remove_error;
      std::filesystem::remove(partial, remove_error);
      if ((!retryable_download(status, code) && status != 401 &&
           !successful_response) ||
          attempt == config_.maximum_attempts) {
        throw std::runtime_error(
            "B2 GIF sweep download or integrity verification failed for " +
            object.key);
      }
      std::this_thread::sleep_for(
          std::chrono::seconds(std::min(60, 1 << (attempt - 1))));
    }
  }

private:
  CurlGlobalGuard curl_global_;
  const gdupe::Config &config_;
  CURL *curl_{};
  std::string authorization_token_;
  std::string download_url_;

  std::string escape(const std::string &value) {
    char *encoded =
        curl_easy_escape(curl_, value.c_str(), static_cast<int>(value.size()));
    if (!encoded)
      throw std::runtime_error("Could not encode a B2 GIF file ID");
    std::string result(encoded);
    curl_free(encoded);
    return result;
  }

  void configure_download(const std::string &url, curl_slist *headers,
                          std::ofstream &stream) {
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 1800L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, "gdupe-gif-sweep/1.0");
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &stream);
  }

  void authorize() {
    for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
      CURL *auth = curl_easy_init();
      if (!auth)
        throw std::runtime_error("Cannot allocate GIF sweep B2 authorization");
      std::string body;
      curl_easy_setopt(auth, CURLOPT_URL,
                       "https://api.backblazeb2.com/b2api/v2/b2_authorize_account");
      curl_easy_setopt(auth, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
      const std::string user_password =
          config_.key_id + ":" + config_.application_key;
      curl_easy_setopt(auth, CURLOPT_USERPWD, user_password.c_str());
      curl_easy_setopt(auth, CURLOPT_CONNECTTIMEOUT, 60L);
      curl_easy_setopt(auth, CURLOPT_TIMEOUT, 1800L);
      curl_easy_setopt(auth, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(auth, CURLOPT_USERAGENT, "gdupe-gif-sweep/1.0");
      curl_easy_setopt(auth, CURLOPT_WRITEFUNCTION, append_to_string);
      curl_easy_setopt(auth, CURLOPT_WRITEDATA, &body);
      const CURLcode code = curl_easy_perform(auth);
      long status = 0;
      curl_easy_getinfo(auth, CURLINFO_RESPONSE_CODE, &status);
      curl_easy_cleanup(auth);

      if (code == CURLE_OK && status >= 200 && status < 300) {
        const auto value = nlohmann::json::parse(body);
        authorization_token_ =
            value.at("authorizationToken").get<std::string>();
        download_url_ = value.at("downloadUrl").get<std::string>();
        const auto allowed = value.value("allowed", nlohmann::json::object());
        bool can_read = false;
        for (const auto &capability :
             allowed.value("capabilities", nlohmann::json::array())) {
          if (capability.is_string() &&
              capability.get<std::string>() == "readFiles") {
            can_read = true;
            break;
          }
        }
        if (!can_read)
          throw std::runtime_error(
              "B2 credentials lack readFiles for the GIF sweep");
        const auto bucket_name = allowed.find("bucketName");
        if (bucket_name != allowed.end() && !bucket_name->is_null()) {
          if (!bucket_name->is_string())
            throw std::runtime_error(
                "B2 returned an invalid bucket restriction for GIF sweep");
          const std::string restricted = bucket_name->get<std::string>();
          if (!restricted.empty() && restricted != config_.bucket_name)
            throw std::runtime_error(
                "B2 credentials are restricted to another bucket");
        }
        return;
      }

      if (!retryable_download(status, code) ||
          attempt == config_.maximum_attempts) {
        throw std::runtime_error("GIF sweep B2 authorization failed (HTTP " +
                                 std::to_string(status) + ")");
      }
      std::this_thread::sleep_for(
          std::chrono::seconds(std::min(30, 1 << (attempt - 1))));
    }
  }
};

struct PreparedGif {
  std::size_t index{};
  std::optional<std::filesystem::path> source;
  bool source_is_temporary{};
  bool reused_saved_source{};
  std::optional<std::string> failure;
};

class GifPrefetcher final {
public:
  GifPrefetcher(const std::vector<gdupe::InventoryObject> &gifs,
                const std::filesystem::path &temp_root,
                const std::filesystem::path &findings_root,
                const gdupe::Config &config)
      : gifs_(gifs), temp_root_(temp_root), findings_root_(findings_root),
        config_(config), worker_([this] { run(); }) {}

  ~GifPrefetcher() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    ready_.notify_all();
    space_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

  GifPrefetcher(const GifPrefetcher &) = delete;
  GifPrefetcher &operator=(const GifPrefetcher &) = delete;

  PreparedGif take(std::size_t expected_index) {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] {
      return worker_failure_ || !queue_.empty() || done_ || stop_;
    });
    if (worker_failure_)
      std::rethrow_exception(worker_failure_);
    if (queue_.empty())
      throw std::runtime_error("GIF prefetcher ended before the sweep did");
    PreparedGif prepared = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();
    space_.notify_one();
    if (prepared.index != expected_index)
      throw std::runtime_error("GIF prefetcher returned an out-of-order item");
    return prepared;
  }

private:
  const std::vector<gdupe::InventoryObject> &gifs_;
  const std::filesystem::path temp_root_;
  const std::filesystem::path findings_root_;
  const gdupe::Config &config_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable space_;
  std::deque<PreparedGif> queue_;
  bool stop_{};
  bool done_{};
  std::exception_ptr worker_failure_;
  std::thread worker_;

  bool should_stop() {
    std::lock_guard lock(mutex_);
    return stop_;
  }

  void enqueue(PreparedGif prepared) {
    std::unique_lock lock(mutex_);
    space_.wait(lock,
                [this] { return stop_ || queue_.size() < kPrefetchDepth; });
    if (stop_)
      return;
    queue_.push_back(std::move(prepared));
    lock.unlock();
    ready_.notify_one();
  }

  void run() {
    try {
      SweepDownloader downloader(config_);
      for (std::size_t index = 0; index < gifs_.size(); ++index) {
        if (should_stop())
          return;
        const auto &object = gifs_[index].remote;
        PreparedGif prepared;
        prepared.index = index;
        try {
          if (auto finding = existing_finding(findings_root_, object)) {
            prepared.source = *finding;
            prepared.reused_saved_source = true;
          } else {
            const auto temporary = temp_root_ / stable_gif_name(object);
            if (std::filesystem::exists(temporary) &&
                exact_local_copy(temporary, object)) {
              prepared.source = temporary;
              prepared.source_is_temporary = true;
            } else {
              if (std::filesystem::exists(temporary))
                remove_tool_file(temporary);
              downloader.download_to(object, temporary);
              prepared.source = temporary;
              prepared.source_is_temporary = true;
            }
          }
        } catch (const std::exception &problem) {
          prepared.failure = problem.what();
        }
        enqueue(std::move(prepared));
      }
      {
        std::lock_guard lock(mutex_);
        done_ = true;
      }
      ready_.notify_all();
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        worker_failure_ = std::current_exception();
        done_ = true;
      }
      ready_.notify_all();
    }
  }
};

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    GdupeProcessLock process_lock;
    const Options options = parse_options(argc, argv);
    const auto executable = executable_path();
    const auto config_path =
        options.config.value_or(gdupe::default_config_path(executable));
    const auto report_path = options.report.value_or(
        executable.parent_path() / "gdupe-gif-sweep-report.jsonl");
    const auto findings_root =
        executable.parent_path() / "gif-sweep-findings";

    auto config = gdupe::Config::load(config_path);
    if (!std::filesystem::exists(config.database_path)) {
      throw std::runtime_error(
          "The local gdupe inventory database is missing. Run gdupe through "
          "a successful B2 synchronization before starting the GIF sweep.");
    }

    gdupe::Database database(config.database_path);
    const std::string inventory_digest =
        database.metadata("last_inventory_sha256");
    if (inventory_digest.empty()) {
      throw std::runtime_error(
          "The local gdupe database has no completed inventory marker. Run "
          "gdupe through a successful B2 synchronization first.");
    }

    const auto local_inventory = database.inventory();
    std::vector<gdupe::InventoryObject> gifs;
    gifs.reserve(local_inventory.size());
    std::size_t already_fingerprinted = 0;
    for (const auto &item : local_inventory) {
      if (item.remote.extension != "gif")
        continue;
      if (has_current_fingerprint(item, config.fingerprint_version)) {
        ++already_fingerprinted;
        continue;
      }
      gifs.push_back(item);
    }

    const auto credentials = gdupe::load_b2_credentials();
    if (!credentials && !gifs.empty()) {
      throw std::runtime_error(
          "No saved B2 credential was found. Run gdupe once first so the "
          "validated credential exists in Windows Credential Manager.");
    }
    if (credentials) {
      config.key_id = credentials->key_id;
      config.application_key = credentials->application_key;
    }

    std::filesystem::create_directories(findings_root / kBrokenDirectory);
    std::filesystem::create_directories(findings_root / kNormalizedDirectory);
    std::ofstream report(report_path, std::ios::trunc);
    if (!report)
      throw std::runtime_error("Could not create the GIF sweep report");

    write_record(report,
                 {{"type", "start"},
                  {"inventory_source", "local_gdupe_database"},
                  {"inventory_digest", inventory_digest},
                  {"database_path", config.database_path.string()},
                  {"gif_count", gifs.size() + already_fingerprinted},
                  {"already_fingerprinted", already_fingerprinted},
                  {"to_sweep", gifs.size()},
                  {"persistent_download_connection", true},
                  {"prefetch_depth", kPrefetchDepth},
                  {"findings_root", findings_root.string()}});

    std::cout << "Using gdupe's completed local inventory.\n"
              << "GIFs: " << (gifs.size() + already_fingerprinted)
              << " | already fingerprinted: " << already_fingerprinted
              << " | to sweep: " << gifs.size() << "\n"
              << "B2 remains read-only. Clean fingerprints are committed "
                 "safely to the local gdupe database and will be reused by "
                 "normal gdupe runs.\n"
              << "Network experiment: one persistent B2 download connection "
                 "with up to "
              << kPrefetchDepth << " verified GIFs prefetched ahead.\n"
              << "Normalized/error GIFs are kept only as source files under: "
              << findings_root.string() << "\n\n";

    const auto temp_root =
        std::filesystem::temp_directory_path() / "gdupe-gif-sweep";
    std::filesystem::create_directories(temp_root);
    gdupe::Fingerprinter fingerprinter(config);
    std::optional<GifPrefetcher> prefetcher;
    if (!gifs.empty())
      prefetcher.emplace(gifs, temp_root, findings_root, config);

    std::size_t clean_saved = 0;
    std::size_t clean_already_present = 0;
    std::size_t normalized = 0;
    std::size_t failed = 0;
    std::size_t reused_saved_source = 0;

    for (std::size_t index = 0; index < gifs.size(); ++index) {
      const auto &item = gifs[index];
      const auto &object = item.remote;
      PreparedGif prepared = prefetcher->take(index);
      std::optional<std::filesystem::path> source = std::move(prepared.source);
      const bool source_is_temporary = prepared.source_is_temporary;
      if (prepared.reused_saved_source)
        ++reused_saved_source;
      std::optional<gdupe::WicGifInfo> inspected;
      std::optional<gdupe::Fingerprint> fingerprint;
      std::optional<std::string> failure = std::move(prepared.failure);

      if (!failure) {
        try {
          if (!source)
            throw std::runtime_error("GIF prefetcher produced no source file");
          {
            gdupe::WicGifDecoder decoder(*source);
            inspected = decoder.info();
          }
          fingerprint = fingerprinter.compute(*source, "gif");
        } catch (const std::exception &problem) {
          failure = problem.what();
        }
      }

      if (failure) {
        ++failed;
        auto record = base_record(object);
        record["status"] = "error";
        record["error"] = *failure;
        if (inspected)
          append_info(record, *inspected);
        if (source && std::filesystem::exists(*source)) {
          try {
            const auto saved = preserve_finding(
                *source, findings_root, kBrokenDirectory, object,
                source_is_temporary);
            record["saved_original"] = saved.string();
          } catch (const std::exception &preservation_problem) {
            record["preservation_error"] = preservation_problem.what();
            write_record(report, record);
            throw;
          }
        }
        write_record(report, record);
        std::cout << "ERROR " << (index + 1) << '/' << gifs.size() << ": "
                  << object.key << " -> " << *failure << '\n';
      } else if (inspected && !inspected->normalizations.empty()) {
        ++normalized;
        const auto saved = preserve_finding(
            *source, findings_root, kNormalizedDirectory, object,
            source_is_temporary);
        auto record = base_record(object);
        record["status"] = "normalized";
        record["saved_original"] = saved.string();
        append_info(record, *inspected);
        write_record(report, record);
        std::cout << "NORMALIZED " << (index + 1) << '/' << gifs.size()
                  << ": " << object.key << " ("
                  << inspected->normalizations.size()
                  << " frame geometry adjustment(s)); fingerprint discarded\n";
      } else {
        const auto current = database.object(object.key);
        if (!current || current->remote.file_id != object.file_id ||
            current->remote.size != object.size ||
            current->remote.sha1 != object.sha1) {
          throw std::runtime_error(
              "The local inventory changed while a clean GIF fingerprint was "
              "being committed; no fingerprint was saved");
        }
        if (has_current_fingerprint(*current, config.fingerprint_version)) {
          ++clean_already_present;
        } else {
          database.save_fingerprint(object.key, object.file_id, *fingerprint);
          verify_saved_fingerprint(database, object, *fingerprint);
          ++clean_saved;
        }
        if (source_is_temporary && source && std::filesystem::exists(*source))
          remove_tool_file(*source);
      }

      const std::size_t processed = index + 1;
      const std::size_t percent_tenths =
          (processed * 1000 + gifs.size() / 2) / gifs.size();
      const std::size_t fingerprinted =
          already_fingerprinted + clean_saved + clean_already_present;
      std::cout << (percent_tenths / 10) << '.' << (percent_tenths % 10)
                << "% | Progress: " << processed << '/' << gifs.size()
                << " | clean_saved=" << clean_saved
                << " normalized=" << normalized << " errors=" << failed
                << " | fingerprinted=" << fingerprinted << std::endl;
      if (processed % 5 == 0)
        print_activity_burst();
    }

    prefetcher.reset();

    write_record(report,
                 {{"type", "summary"},
                  {"total_gifs", gifs.size() + already_fingerprinted},
                  {"already_fingerprinted", already_fingerprinted},
                  {"swept", gifs.size()},
                  {"clean_saved", clean_saved},
                  {"clean_already_present", clean_already_present},
                  {"normalized", normalized},
                  {"errors", failed},
                  {"reused_saved_source", reused_saved_source},
                  {"persistent_download_connection", true},
                  {"prefetch_depth", kPrefetchDepth},
                  {"findings_root", findings_root.string()}});

    std::error_code remove_error;
    std::filesystem::remove(temp_root, remove_error);

    std::cout << "\nDone. Report: " << report_path.string() << '\n'
              << "Already fingerprinted: " << already_fingerprinted
              << " | New clean fingerprints saved: " << clean_saved
              << " | Normalized (not saved to DB): " << normalized
              << " | Errors: " << failed << '\n'
              << "Special GIF originals: " << findings_root.string() << '\n';
    return failed == 0 ? 0 : 2;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe GIF sweep failed: " << problem.what() << '\n';
    return 1;
  }
}
