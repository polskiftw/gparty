#include "boot_install.hpp"
#include "config.hpp"
#include "control_window.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "fingerprint.hpp"
#include "nvdec_decode.hpp"
#include "readonly_b2.hpp"
#include "registry.hpp"
#include "runtime_stats.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace fp = gparty::fingerprints;

namespace {

constexpr wchar_t kMutexName[] = L"Global\\GPartyGfingerdWorker";
constexpr wchar_t kStopEventName[] = L"Global\\GPartyGfingerdStop";
std::atomic_bool stop_requested{};
HANDLE worker_stop_event{};

class NamedObjectSecurity {
public:
  NamedObjectSecurity() {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;BU)", SDDL_REVISION_1,
            &descriptor_, nullptr))
      throw std::runtime_error(
          "Windows could not create the worker synchronization policy");
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
  }
  ~NamedObjectSecurity() {
    if (descriptor_)
      LocalFree(descriptor_);
  }
  SECURITY_ATTRIBUTES *attributes() { return &attributes_; }

private:
  PSECURITY_DESCRIPTOR descriptor_{};
  SECURITY_ATTRIBUTES attributes_{};
};

bool stopping() {
  return stop_requested ||
         (worker_stop_event &&
          WaitForSingleObject(worker_stop_event, 0) == WAIT_OBJECT_0);
}

class WorkerStopEvent {
public:
  WorkerStopEvent() {
    NamedObjectSecurity security;
    handle_ = CreateEventW(security.attributes(), TRUE, FALSE, kStopEventName);
    if (!handle_)
      throw std::runtime_error("Windows could not create the worker stop event");
    ResetEvent(handle_);
    worker_stop_event = handle_;
  }
  ~WorkerStopEvent() {
    worker_stop_event = nullptr;
    if (handle_)
      CloseHandle(handle_);
  }

private:
  HANDLE handle_{};
};

class InstanceMutex {
public:
  InstanceMutex() {
    NamedObjectSecurity security;
    handle_ = CreateMutexW(security.attributes(), FALSE, kMutexName);
    if (!handle_)
      throw std::runtime_error("Windows could not create the instance mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error("gfingerd is already running");
    }
  }
  ~InstanceMutex() {
    if (handle_)
      CloseHandle(handle_);
  }
  InstanceMutex(const InstanceMutex &) = delete;
  InstanceMutex &operator=(const InstanceMutex &) = delete;

private:
  HANDLE handle_{};
};

class Logger {
public:
  explicit Logger(std::filesystem::path path, bool mirror_to_console = false)
      : path_(std::move(path)), mirror_to_console_(mirror_to_console) {
    if (!path_.parent_path().empty())
      std::filesystem::create_directories(path_.parent_path());
    rotate();
  }

  void write(const std::string &message) {
    std::scoped_lock lock(mutex_);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::ostringstream line;
    line << time.wYear << '-';
    line.width(2);
    line.fill('0');
    line << time.wMonth << '-';
    line.width(2);
    line << time.wDay << ' ';
    line.width(2);
    line << time.wHour << ':';
    line.width(2);
    line << time.wMinute << ':';
    line.width(2);
    line << time.wSecond << "  " << message;
    std::ofstream stream(path_, std::ios::app);
    if (stream)
      stream << line.str() << '\n';
    if (mirror_to_console_)
      std::cout << line.str() << '\n' << std::flush;
  }

private:
  void rotate() {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) ||
        std::filesystem::file_size(path_, error) < 4 * 1024 * 1024)
      return;
    const auto one = path_.string() + ".1";
    const auto two = path_.string() + ".2";
    std::filesystem::remove(two, error);
    if (std::filesystem::exists(one, error))
      std::filesystem::rename(one, two, error);
    std::filesystem::rename(path_, one, error);
  }

  std::filesystem::path path_;
  bool mirror_to_console_{};
  std::mutex mutex_;
};

std::filesystem::path executable_path() {
  std::wstring buffer(32'768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size())
    throw std::runtime_error("Windows could not resolve the executable path");
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::wstring quote_argument(const std::wstring &value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos)
    return value;
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring command_line(const std::vector<std::wstring> &arguments) {
  std::wstring command;
  for (const auto &argument : arguments) {
    if (!command.empty())
      command.push_back(L' ');
    command += quote_argument(argument);
  }
  return command;
}

void request_worker_stop() {
  const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (!event)
    return;
  SetEvent(event);
  CloseHandle(event);
}

void start_live_viewer(const std::filesystem::path &executable,
                       const std::optional<std::filesystem::path> &config_path) {
  std::vector<std::wstring> arguments{executable.wstring(), L"--viewer"};
  if (config_path) {
    arguments.push_back(L"--config");
    arguments.push_back(config_path->wstring());
  }
  std::wstring command = command_line(arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process))
    throw std::runtime_error("Windows could not open the live CMD output");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

std::string read_secret() {
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool console = input != INVALID_HANDLE_VALUE &&
                       GetConsoleMode(input, &mode) != FALSE;
  if (console)
    SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
  std::string value;
  std::getline(std::cin, value);
  if (console) {
    SetConsoleMode(input, mode);
    std::cout << '\n';
  }
  return value;
}

bool same_identity(const gdupe::RemoteObject &first,
                   const gdupe::RemoteObject &second) {
  if (first.key != second.key || first.file_id != second.file_id ||
      first.size != second.size)
    return false;
  const bool first_sha1 = first.sha1.size() == 40;
  const bool second_sha1 = second.sha1.size() == 40;
  return !first_sha1 || !second_sha1 || first.sha1 == second.sha1;
}

void clean_partials(const std::filesystem::path &cache) {
  std::filesystem::create_directories(cache);
  for (const auto &entry : std::filesystem::directory_iterator(cache))
    if (entry.is_regular_file() && entry.path().extension() == ".partial")
      std::filesystem::remove(entry.path());
}

bool requires_nvdec(std::string_view extension) {
  return extension == "mp4" || extension == "m4v" || extension == "webm";
}

bool is_known_malformed_gif_geometry(std::string_view extension,
                                     std::string_view error) {
  return extension == "gif" &&
         error == "GIF frame rectangle is outside its logical canvas";
}

std::filesystem::path deferred_gif_path(const fp::Config &config,
                                        const gdupe::RemoteObject &object) {
  return config.cache_directory.parent_path() / "deferred-gifs" /
         (gdupe::sha256(object.file_id) + ".gif");
}

void write_deferred_gif_note(const std::filesystem::path &gif_path,
                             const gdupe::RemoteObject &object,
                             std::string_view reason) {
  const nlohmann::json note{
      {"state", "deferred-malformed-gif"},
      {"key", object.key},
      {"file_id", object.file_id},
      {"size", object.size},
      {"sha1", object.sha1},
      {"extension", object.extension},
      {"local_gif", gif_path.string()},
      {"reason", reason},
      {"todo", "Reprocess after malformed GIF geometry support is fixed"}};
  std::ofstream stream(gif_path.string() + ".json", std::ios::trunc);
  if (!stream)
    throw std::runtime_error("Could not write deferred GIF recovery note");
  stream << note.dump(2) << '\n';
  if (!stream)
    throw std::runtime_error("Could not finish deferred GIF recovery note");
}

std::filesystem::path cache_path(const fp::Config &config,
                                 const gdupe::RemoteObject &object) {
  return config.cache_directory /
         (gdupe::sha256(object.file_id) + "." + object.extension);
}

std::filesystem::path runtime_status_path(const fp::Config &config) {
  return config.log_path.parent_path() / "gfingerd-status.json";
}

void print_status(const fp::Config &config) {
  fp::Registry registry(config.database_path);
  const auto status = registry.status();
  std::cout << "Database: " << config.database_path.string() << '\n'
            << "Inventory objects: " << status.inventory_objects << '\n'
            << "Fully fingerprinted: " << status.fully_fingerprinted << '\n'
            << "Pending objects: " << status.pending_objects << '\n'
            << "Unsupported: " << status.unsupported << '\n'
            << "Deferred malformed GIFs: " << status.deferred_gifs << '\n'
            << "Failed: " << status.failed << '\n'
            << "Last successful scan: "
            << (status.last_successful_scan.empty() ? "never"
                                                    : status.last_successful_scan)
            << '\n';
}

bool worker_is_reporting(const fp::Config &config) {
  try {
    std::ifstream stream(runtime_status_path(config));
    nlohmann::json value;
    stream >> value;
    const auto updated = value.value("updated_unix_ms", 0LL);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return updated > 0 && now - updated < 10'000 &&
           value.value("state", std::string{}) != "stopped";
  } catch (...) {
    return false;
  }
}

void print_live_stats(const fp::Config &config) {
  try {
    std::ifstream stream(runtime_status_path(config));
    nlohmann::json value;
    stream >> value;
    const double speed =
        value.value("bytes_per_second", 0.0) / (1024.0 * 1024.0);
    const double session =
        static_cast<double>(value.value("session_bytes", 0ULL)) /
        (1024.0 * 1024.0 * 1024.0);
    std::cout << "[pipeline] downloads " << value.value("active_downloads", 0)
              << '/'
              << value.value("configured_download_connections",
                             config.download_connections)
              << " | ready " << value.value("prefetch_ready", 0) << '/'
              << value.value("prefetch_capacity", config.prefetch_files)
              << " | fingerprints "
              << value.value("active_fingerprint_workers", 0) << '/'
              << value.value("configured_fingerprint_workers",
                             config.worker_threads)
              << " | " << std::fixed << std::setprecision(1) << speed
              << " MB/s | " << session << " GB session | "
              << value.value("completed_session", 0ULL) << " completed, "
              << value.value("failed_session", 0ULL) << " failed\n"
              << std::defaultfloat << std::flush;
  } catch (...) {
  }
}

void follow_log(const fp::Config &config) {
  std::cout << "\nLive gfingerd activity (Ctrl+C closes this viewer only)\n"
            << "Log: " << config.log_path.string() << "\n\n";
  std::uintmax_t offset = 0;
  int missing_worker_checks = 0;
  int stats_checks = 0;
  while (!stopping()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::error_code error;
    if (std::filesystem::is_regular_file(config.log_path, error)) {
      const auto size = std::filesystem::file_size(config.log_path, error);
      if (!error) {
        if (size < offset)
          offset = 0;
        if (size > offset) {
          std::ifstream stream(config.log_path, std::ios::binary);
          stream.seekg(static_cast<std::streamoff>(offset));
          std::string line;
          while (std::getline(stream, line))
            std::cout << line << '\n';
          std::cout << std::flush;
          offset = size;
        }
      }
    }
    if (++stats_checks >= 10) {
      print_live_stats(config);
      stats_checks = 0;
    }
    if (worker_is_reporting(config)) {
      missing_worker_checks = 0;
    } else if (++missing_worker_checks >= 60) {
      std::cout << "\ngfingerd stopped.\n";
      return;
    }
  }
}

struct DownloadedItem {
  fp::PendingObject item;
  std::filesystem::path path;
};

class ReadyQueue {
public:
  ReadyQueue(std::size_t capacity, std::size_t producer_count,
             fp::RuntimeStats &stats)
      : capacity_(capacity), producer_count_(producer_count), stats_(stats) {}

  bool push(DownloadedItem item) {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] {
      return stopped_ || stopping() || items_.size() < capacity_;
    });
    if (stopped_ || stopping())
      return false;
    items_.push_back(std::move(item));
    stats_.set_prefetch_ready(items_.size());
    condition_.notify_all();
    return true;
  }

  std::optional<DownloadedItem> pop() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] {
      return stopped_ || stopping() || !items_.empty() ||
             producers_done_ >= producer_count_;
    });
    if (stopped_ || stopping() || items_.empty())
      return std::nullopt;
    DownloadedItem item = std::move(items_.front());
    items_.pop_front();
    stats_.set_prefetch_ready(items_.size());
    condition_.notify_all();
    return item;
  }

  void producer_done() {
    std::scoped_lock lock(mutex_);
    ++producers_done_;
    condition_.notify_all();
  }

  void request_stop() {
    std::scoped_lock lock(mutex_);
    stopped_ = true;
    condition_.notify_all();
  }

  void remove_staged_files() {
    std::deque<DownloadedItem> remaining;
    {
      std::scoped_lock lock(mutex_);
      remaining.swap(items_);
      stats_.set_prefetch_ready(0);
    }
    for (const auto &item : remaining) {
      std::error_code ignored;
      std::filesystem::remove(item.path, ignored);
    }
  }

private:
  std::size_t capacity_{};
  std::size_t producer_count_{};
  fp::RuntimeStats &stats_;
  std::deque<DownloadedItem> items_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t producers_done_{};
  bool stopped_{};
};

class Worker {
public:
  Worker(fp::Config config, Logger &logger, fp::RuntimeStats &stats)
      : config_(std::move(config)), logger_(logger),
        registry_(config_.database_path), b2_(config_), stats_(stats),
        download_clients_(static_cast<std::size_t>(config_.download_connections)),
        lookup_clients_(static_cast<std::size_t>(config_.worker_threads)) {
    clean_partials(config_.cache_directory);
  }

  bool cycle(bool nvdec_ready) {
    stats_.set_state("scanning");
    logger_.write("Listing canonical B2 inventory");
    const auto inventory = b2_.list_objects(config_.canonical_prefix);
    registry_.reconcile(inventory);

    const auto pending = registry_.pending();
    std::vector<fp::PendingObject> work;
    work.reserve(pending.size());
    bool video_deferred = false;
    for (const auto &item : pending) {
      if (requires_nvdec(item.remote.extension) && !nvdec_ready) {
        video_deferred = true;
        continue;
      }
      work.push_back(item);
    }
    logger_.write("Inventory=" + std::to_string(inventory.size()) +
                  " pending=" + std::to_string(pending.size()) +
                  " eligible_now=" + std::to_string(work.size()));
    if (work.empty()) {
      stats_.set_prefetch_ready(0);
      stats_.set_state(video_deferred
                           ? "idle; videos waiting for NVIDIA driver"
                           : "idle");
      return video_deferred;
    }

    stats_.set_state("downloading + fingerprinting");
    const auto download_count =
        (std::min)(work.size(),
                   static_cast<std::size_t>(config_.download_connections));
    const auto fingerprint_count =
        (std::min)(work.size(), static_cast<std::size_t>(config_.worker_threads));
    ReadyQueue ready(static_cast<std::size_t>(config_.prefetch_files),
                     download_count, stats_);
    std::atomic_size_t next_download{};
    std::atomic_bool infrastructure_failed{};
    std::jthread heartbeat([&](std::stop_token token) {
      while (!token.stop_requested()) {
        stats_.heartbeat();
        if (stopping())
          ready.request_stop();
        for (int tenth = 0; tenth < 10 && !token.stop_requested(); ++tenth)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    std::vector<std::thread> downloaders;
    downloaders.reserve(download_count);
    for (std::size_t connection = 0; connection < download_count; ++connection) {
      downloaders.emplace_back([&, connection] {
        try {
          if (!download_clients_[connection])
            download_clients_[connection] =
                std::make_unique<fp::ReadOnlyB2Client>(config_);
          fp::Registry registry(config_.database_path);
          while (!stopping() && !infrastructure_failed.load()) {
            const auto index = next_download.fetch_add(1);
            if (index >= work.size())
              break;
            const auto &item = work[index];
            const auto path = cache_path(config_, item.remote);
            stats_.download_begin(connection, item.remote.key);
            bool downloaded = false;
            try {
              download_clients_[connection]->download_to(
                  item.remote, path,
                  [&](std::uint64_t transferred, std::uint64_t total) {
                    stats_.download_progress(connection, transferred, total);
                    return !stopping();
                  });
              downloaded = true;
            } catch (const fp::B2InfrastructureError &problem) {
              infrastructure_failed = true;
              logger_.write("B2 infrastructure failure; item remains pending: " +
                            item.remote.key + " -- " + problem.what());
            } catch (const std::exception &problem) {
              if (!stopping()) {
                registry.record_failure(item.remote, problem.what(),
                                        config_.maximum_item_attempts);
                stats_.item_failed();
                logger_.write("Download failed without stopping backlog: " +
                              item.remote.key + " -- " + problem.what());
              }
            }
            stats_.download_finish(connection);
            if (!downloaded) {
              std::error_code ignored;
              std::filesystem::remove(path, ignored);
              if (infrastructure_failed.load()) {
                ready.request_stop();
                break;
              }
              continue;
            }
            if (!ready.push({item, path})) {
              std::error_code ignored;
              std::filesystem::remove(path, ignored);
              break;
            }
          }
        } catch (const std::exception &problem) {
          infrastructure_failed = true;
          logger_.write("Downloader thread stopped: " +
                        std::string(problem.what()));
          ready.request_stop();
        }
        stats_.download_finish(connection);
        ready.producer_done();
      });
    }

    std::vector<std::thread> fingerprint_threads;
    fingerprint_threads.reserve(fingerprint_count);
    for (std::size_t worker_index = 0; worker_index < fingerprint_count;
         ++worker_index) {
      fingerprint_threads.emplace_back([&, worker_index] {
        try {
          fp::Registry registry(config_.database_path);
          if (!lookup_clients_[worker_index])
            lookup_clients_[worker_index] =
                std::make_unique<fp::ReadOnlyB2Client>(config_);
          const auto gdupe_config = fingerprint_config();
          gdupe::Fingerprinter fingerprinter(gdupe_config);
          while (!stopping()) {
            auto downloaded = ready.pop();
            if (!downloaded)
              break;
            process_downloaded(*downloaded, worker_index, registry,
                               *lookup_clients_[worker_index], fingerprinter,
                               infrastructure_failed);
            if (infrastructure_failed.load()) {
              ready.request_stop();
              break;
            }
          }
        } catch (const std::exception &problem) {
          infrastructure_failed = true;
          logger_.write("Fingerprint worker stopped: " +
                        std::string(problem.what()));
          ready.request_stop();
        }
        stats_.fingerprint_finish(worker_index, false, false);
      });
    }

    for (auto &thread : downloaders)
      thread.join();
    for (auto &thread : fingerprint_threads)
      thread.join();
    ready.request_stop();
    ready.remove_staged_files();
    heartbeat.request_stop();

    if (stopping()) {
      stats_.set_state("stopping");
      return video_deferred;
    }
    if (infrastructure_failed.load())
      throw fp::B2InfrastructureError(
          "B2 pipeline had a transient network/service failure");
    stats_.set_state(video_deferred
                         ? "idle; videos waiting for NVIDIA driver"
                         : "idle");
    return video_deferred;
  }

private:
  gdupe::Config fingerprint_config() const {
    gdupe::Config config;
    config.bucket_name = config_.bucket_name;
    config.cache_directory = config_.cache_directory;
    config.fingerprint_version = fp::kFingerprintVersion;
    config.video_sample_frames = fp::kVideoSampleFrames;
    config.gif_sample_frames = fp::kGifSampleFrames;
    return config;
  }

  void process_downloaded(const DownloadedItem &downloaded,
                          std::size_t worker_index, fp::Registry &registry,
                          fp::ReadOnlyB2Client &b2,
                          gdupe::Fingerprinter &fingerprinter,
                          std::atomic_bool &infrastructure_failed) {
    const auto &item = downloaded.item;
    const auto &path = downloaded.path;
    stats_.fingerprint_begin(worker_index, item.remote.key);
    logger_.write("Fingerprinting " + item.remote.key);
    bool completed = false;
    bool failed = false;
    bool keep_local_file = false;
    try {
      const auto fingerprint = fingerprinter.compute(path, item.remote.extension);
      const auto latest = b2.find_object(item.remote.key);
      if (!latest || !same_identity(*latest, item.remote)) {
        logger_.write("Discarded stale result for changed object " +
                      item.remote.key);
      } else {
        registry.save_fingerprint(item.remote, fingerprint);
        completed = true;
      }
    } catch (const fp::B2InfrastructureError &problem) {
      infrastructure_failed = true;
      logger_.write("B2 verification unavailable; item remains pending: " +
                    item.remote.key + " -- " + problem.what());
    } catch (const std::exception &problem) {
      if (!stopping() &&
          is_known_malformed_gif_geometry(item.remote.extension,
                                          problem.what())) {
        try {
          const auto latest = b2.find_object(item.remote.key);
          if (!latest || !same_identity(*latest, item.remote)) {
            logger_.write("Malformed GIF changed before deferral; discarded: " +
                          item.remote.key);
          } else {
            const auto deferred_path = deferred_gif_path(config_, item.remote);
            std::filesystem::create_directories(deferred_path.parent_path());
            std::error_code move_error;
            std::filesystem::remove(deferred_path, move_error);
            move_error.clear();
            std::filesystem::rename(path, deferred_path, move_error);
            if (move_error) {
              std::filesystem::copy_file(
                  path, deferred_path,
                  std::filesystem::copy_options::overwrite_existing);
              std::filesystem::remove(path);
            }
            write_deferred_gif_note(deferred_path, item.remote, problem.what());
            registry.defer_gif(item.remote, deferred_path, problem.what());
            keep_local_file = true;
            logger_.write("Deferred malformed GIF: " + item.remote.key +
                          " -> " + deferred_path.string());
          }
        } catch (const fp::B2InfrastructureError &verification_problem) {
          infrastructure_failed = true;
          logger_.write("B2 verification unavailable while deferring GIF: " +
                        item.remote.key + " -- " +
                        verification_problem.what());
        } catch (const std::exception &defer_problem) {
          if (!stopping()) {
            registry.record_failure(item.remote, defer_problem.what(),
                                    config_.maximum_item_attempts);
            failed = true;
            logger_.write("Could not preserve malformed GIF: " +
                          item.remote.key + " -- " + defer_problem.what());
          }
        }
      } else if (!stopping()) {
        registry.record_failure(item.remote, problem.what(),
                                config_.maximum_item_attempts);
        failed = true;
        logger_.write("Item failed without stopping backlog: " +
                      item.remote.key + " -- " + problem.what());
      }
    }
    if (!keep_local_file) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
    stats_.fingerprint_finish(worker_index,
                              !stopping() && completed,
                              !stopping() && failed);
  }

  fp::Config config_;
  Logger &logger_;
  fp::Registry registry_;
  fp::ReadOnlyB2Client b2_;
  fp::RuntimeStats &stats_;
  std::vector<std::unique_ptr<fp::ReadOnlyB2Client>> download_clients_;
  std::vector<std::unique_ptr<fp::ReadOnlyB2Client>> lookup_clients_;
};

BOOL WINAPI control_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
    stop_requested = true;
    if (worker_stop_event)
      SetEvent(worker_stop_event);
    return TRUE;
  }
  return FALSE;
}

std::optional<std::filesystem::path>
argument_value(const std::vector<std::string> &arguments,
               const std::string &name) {
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
    if (arguments[index] == name)
      return std::filesystem::path(arguments[index + 1]);
  return std::nullopt;
}

bool has_argument(const std::vector<std::string> &arguments,
                  const std::string &name) {
  return std::find(arguments.begin(), arguments.end(), name) != arguments.end();
}

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  const auto boot_install_payload = argument_value(arguments, "--install-boot");
  const bool gui_mode = arguments.empty();
  if (gui_mode) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (GetConsoleWindow())
      ShowWindow(GetConsoleWindow(), SW_HIDE);
  }
  try {
    const auto executable = executable_path();
    if (boot_install_payload) {
      fp::install_boot_worker(executable, *boot_install_payload);
      return 0;
    }
    if (has_argument(arguments, "--uninstall-boot")) {
      fp::uninstall_boot_worker();
      return 0;
    }

    const bool boot_worker = has_argument(arguments, "--boot-worker");
    const auto config_path = argument_value(arguments, "--config");
    fp::Config config = boot_worker
                            ? fp::Config::load(fp::machine_config_path())
                            : (config_path ? fp::Config::load(*config_path)
                                           : fp::Config::load_user_or_defaults());

    if (gui_mode) {
      auto credentials = fp::load_credentials();
      bool startup = fp::boot_worker_installed();
      while (true) {
        const auto result = fp::show_control_window(
            GetModuleHandleW(nullptr), config, credentials, startup,
            "Loading live status...");
        if (result.action == fp::ControlAction::close)
          return 0;
        if (result.action == fp::ControlAction::live_output) {
          if (!credentials) {
            fp::show_control_error(
                "Save a valid read-only B2 key before opening live output.");
            continue;
          }
          if (!startup) {
            fp::show_control_information(
                "The boot worker is not installed. Use Save & Start first.");
            continue;
          }
          start_live_viewer(executable, config_path);
          continue;
        }

        config = result.config;
        credentials = result.credentials;
        try {
          if (!credentials)
            throw std::runtime_error(
                "The configuration window returned no B2 key");
          config.key_id = credentials->key_id;
          config.application_key = credentials->application_key;
          fp::ReadOnlyB2Client validation(config);
          fp::store_credentials(*credentials);
          if (!config_path)
            config.save_user();
          request_worker_stop();
          if (result.autostart) {
            const auto payload =
                fp::create_boot_install_payload(config, *credentials);
            fp::run_elevated_boot_action(executable, L"--install-boot", payload);
          } else if (startup) {
            fp::run_elevated_boot_action(executable, L"--uninstall-boot");
          }
          startup = result.autostart;
          fp::show_control_information(
              startup
                  ? "Configuration saved. gfingerd now starts at boot before Windows login."
                  : "Configuration saved. Automatic boot fingerprinting is off.");
        } catch (const std::exception &problem) {
          fp::show_control_error(problem.what());
        }
      }
    }

    if (has_argument(arguments, "--status")) {
      print_status(config);
      return 0;
    }

    const bool daemon = has_argument(arguments, "--daemon") || boot_worker;
    if (daemon && GetConsoleWindow())
      ShowWindow(GetConsoleWindow(), SW_HIDE);

    if (has_argument(arguments, "--viewer")) {
      SetConsoleCtrlHandler(control_handler, TRUE);
      follow_log(config);
      return 0;
    }

    const bool set_credentials = has_argument(arguments, "--set-credentials");
    auto credentials = set_credentials
                           ? std::optional<fp::B2Credentials>{}
                           : (boot_worker
                                  ? std::optional<fp::B2Credentials>{
                                        fp::load_machine_credentials()}
                                  : fp::load_credentials());
    bool newly_entered = false;
    if (!credentials) {
      if (daemon)
        throw std::runtime_error("No read-only B2 login is configured");
      std::cout << "gfingerd - B2 setup\n\nB2 Key ID: ";
      std::string key_id;
      std::getline(std::cin, key_id);
      std::cout << "B2 Application Key: ";
      std::string application_key = read_secret();
      credentials = fp::B2Credentials{std::move(key_id),
                                      std::move(application_key)};
      newly_entered = true;
    }
    config.key_id = credentials->key_id;
    config.application_key = credentials->application_key;
    if (newly_entered) {
      fp::ReadOnlyB2Client validation(config);
      fp::store_credentials(*credentials);
    }
    if (set_credentials) {
      std::cout << "The read/list B2 login was validated and saved.\n";
      return 0;
    }

    InstanceMutex instance;
    WorkerStopEvent stop_event;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler(control_handler, TRUE);
    Logger logger(config.log_path, !daemon);
    fp::RuntimeStats stats(
        runtime_status_path(config),
        static_cast<std::size_t>(config.download_connections),
        static_cast<std::size_t>(config.worker_threads),
        static_cast<std::size_t>(config.prefetch_files), boot_worker);
    logger.write(boot_worker
                     ? "Startup worker launched by Windows Task Scheduler"
                     : "Worker launched manually");
    std::unique_ptr<Worker> worker;
    bool nvdec_ready_logged = false;
    const bool once = has_argument(arguments, "--once");
    int consecutive_failures = 0;
    bool videos_waiting_for_nvdec = false;
    do {
      try {
        const bool nvdec_ready = gdupe::nvdec_runtime_available();
        if (nvdec_ready && !nvdec_ready_logged) {
          stats.mark_nvdec_ready();
          logger.write("NVIDIA driver and NVDEC runtime are ready");
          nvdec_ready_logged = true;
        }
        stats.set_state("connecting");
        if (!worker)
          worker = std::make_unique<Worker>(config, logger, stats);
        videos_waiting_for_nvdec = worker->cycle(nvdec_ready);
        consecutive_failures = 0;
      } catch (const std::exception &problem) {
        ++consecutive_failures;
        logger.write(std::string("Cycle failed: ") + problem.what());
        worker.reset();
        if (once)
          throw;
      }
      if (once)
        break;
      const int delay = consecutive_failures == 0
                            ? (videos_waiting_for_nvdec ? 15
                                                       : config.polling_seconds)
                            : (std::min)(
                                  600, 15 << (std::min)(5, consecutive_failures - 1));
      for (int second = 0; second < delay && !stopping(); ++second) {
        stats.heartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    } while (!stopping());
    return 0;
  } catch (const std::exception &problem) {
    if (boot_install_payload)
      fp::report_boot_install_error(*boot_install_payload, problem.what());
    if (gui_mode)
      fp::show_control_error(problem.what());
    else
      std::cerr << "gfingerd: " << problem.what() << '\n';
    return 1;
  }
}
