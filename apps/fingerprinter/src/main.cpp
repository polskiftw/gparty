#include "config.hpp"
#include "control_window.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "fingerprint.hpp"
#include "readonly_b2.hpp"
#include "registry.hpp"
#include "runtime_stats.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace fp = gparty::fingerprints;

namespace {

constexpr wchar_t kMutexName[] = L"Local\\GPartyFingerprintRegistryWorker";
constexpr wchar_t kStopEventName[] = L"Local\\GPartyFingerprintRegistryStop";
constexpr wchar_t kTaskName[] = L"GParty Background Fingerprinter";
std::atomic_bool stop_requested{};
HANDLE worker_stop_event{};

bool stopping() {
  return stop_requested ||
         (worker_stop_event &&
          WaitForSingleObject(worker_stop_event, 0) == WAIT_OBJECT_0);
}

class WorkerStopEvent {
public:
  WorkerStopEvent() {
    handle_ = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
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
    handle_ = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!handle_)
      throw std::runtime_error("Windows could not create the instance mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error(
          "The GParty background fingerprinter is already running");
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

int run_process(const std::vector<std::wstring> &arguments) {
  std::wstring command = command_line(arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    throw std::runtime_error("Windows could not start schtasks.exe");
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

bool worker_is_running() {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
  if (!mutex)
    return false;
  CloseHandle(mutex);
  return true;
}

void start_daemon(const std::filesystem::path &executable,
                  const std::optional<std::filesystem::path> &config_path) {
  std::vector<std::wstring> arguments{executable.wstring(), L"--daemon"};
  if (config_path) {
    arguments.push_back(L"--config");
    arguments.push_back(config_path->wstring());
  }
  std::wstring command = command_line(arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    throw std::runtime_error("Windows could not start the background worker");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void request_worker_stop() {
  const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (!event)
    return;
  SetEvent(event);
  CloseHandle(event);
}

void restart_daemon(const std::filesystem::path &executable,
                    const std::optional<std::filesystem::path> &config_path) {
  request_worker_stop();
  std::vector<std::wstring> arguments{executable.wstring(),
                                      L"--restart-daemon"};
  if (config_path) {
    arguments.push_back(L"--config");
    arguments.push_back(config_path->wstring());
  }
  std::wstring command = command_line(arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    throw std::runtime_error("Windows could not restart the background worker");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void install_autostart(const std::filesystem::path &executable) {
  const std::wstring action =
      L"\"" + executable.wstring() + L"\" --daemon";
  const int code = run_process(
      {L"schtasks.exe", L"/Create", L"/F", L"/SC", L"ONLOGON", L"/DELAY",
       L"0000:30", L"/RL", L"LIMITED", L"/TN", kTaskName, L"/TR", action});
  if (code != 0)
    throw std::runtime_error("Task Scheduler rejected the autostart task");
}

void remove_autostart() {
  const int code =
      run_process({L"schtasks.exe", L"/Delete", L"/F", L"/TN", kTaskName});
  if (code != 0)
    throw std::runtime_error("Task Scheduler could not remove the task");
}

bool autostart_installed() {
  return run_process({L"schtasks.exe", L"/Query", L"/TN", kTaskName}) == 0;
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
  for (const auto &entry : std::filesystem::directory_iterator(cache)) {
    if (entry.is_regular_file() && entry.path().extension() == ".partial")
      std::filesystem::remove(entry.path());
  }
}

std::filesystem::path cache_path(const fp::Config &config,
                                 const gdupe::RemoteObject &object) {
  return config.cache_directory /
         (gdupe::sha256(object.file_id) + "." + object.extension);
}

void print_status(const fp::Config &config) {
  fp::Registry registry(config.database_path);
  const auto status = registry.status();
  std::cout << "Database: " << config.database_path.string() << '\n'
            << "Inventory objects: " << status.inventory_objects << '\n'
            << "Fully fingerprinted: " << status.fully_fingerprinted << '\n'
            << "Pending objects: " << status.pending_objects << '\n'
            << "Pending components: " << status.pending_components << '\n'
            << "Unsupported: " << status.unsupported << '\n'
            << "Failed: " << status.failed << '\n'
            << "Last successful scan: "
            << (status.last_successful_scan.empty()
                    ? "never"
                    : status.last_successful_scan)
            << '\n'
            << "Currently processing: "
            << (status.currently_processing.empty()
                    ? "idle"
                    : status.currently_processing)
            << '\n';
}

std::filesystem::path runtime_status_path(const fp::Config &config) {
  return config.log_path.parent_path() / "fingerprinter-status.json";
}

void print_live_stats(const fp::Config &config) {
  try {
    std::ifstream stream(runtime_status_path(config));
    nlohmann::json value;
    stream >> value;
    const double speed = value.value("bytes_per_second", 0.0) / (1024.0 * 1024.0);
    const double session =
        static_cast<double>(value.value("session_bytes", 0ULL)) /
        (1024.0 * 1024.0 * 1024.0);
    std::cout << "[stats] workers " << value.value("active_workers", 0) << '/'
              << value.value("configured_workers", config.worker_threads)
              << " | " << std::fixed << std::setprecision(1) << speed
              << " MB/s | " << session << " GB this session | "
              << value.value("completed_session", 0ULL) << " completed\n"
              << std::defaultfloat << std::flush;
  } catch (...) {
  }
}

void print_recent_log(const std::filesystem::path &path,
                      std::uintmax_t &offset) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    offset = 0;
    return;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    offset = 0;
    return;
  }
  constexpr std::uintmax_t maximum_history = 64 * 1024;
  const auto start = size > maximum_history ? size - maximum_history : 0;
  std::ifstream stream(path, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(start));
  std::string line;
  if (start != 0)
    std::getline(stream, line);
  std::deque<std::string> recent;
  while (std::getline(stream, line)) {
    recent.push_back(line);
    if (recent.size() > 30)
      recent.pop_front();
  }
  for (const auto &entry : recent)
    std::cout << entry << '\n';
  offset = size;
}

void follow_log(const fp::Config &config) {
  std::cout << "\nLive background activity (Ctrl+C closes this viewer only)\n"
            << "Log: " << config.log_path.string() << "\n\n";
  std::uintmax_t offset = 0;
  print_recent_log(config.log_path, offset);
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
    if (worker_is_running()) {
      missing_worker_checks = 0;
    } else if (++missing_worker_checks >= 60) {
      std::cout << "\nThe background worker stopped. Reopen this EXE to restart it.\n";
      return;
    }
  }
  std::cout << "\nViewer closed. Background fingerprinting is still running.\n";
}

void wait_for_worker_start() {
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (worker_is_running())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error(
      "The background worker did not start; reopen this EXE to try again");
}

class Worker {
public:
  Worker(fp::Config config, Logger &logger, fp::RuntimeStats &stats)
      : config_(std::move(config)), logger_(logger), registry_(config_.database_path),
        b2_(config_), stats_(stats) {
    clean_partials(config_.cache_directory);
  }

  void cycle() {
    stats_.set_state("scanning");
    logger_.write("Listing canonical B2 inventory");
    const auto inventory = b2_.list_objects(config_.canonical_prefix);
    registry_.reconcile(inventory);
    if (!adoption_attempted_) {
      const auto adopted =
          registry_.adopt_gdupe_v3(config_.legacy_gdupe_database);
      logger_.write("Existing corpus adoption: scanned=" +
                    std::to_string(adopted.rows_scanned) +
                    " matching=" + std::to_string(adopted.matching_rows) +
                    " imported_components=" +
                    std::to_string(adopted.components_imported));
      adoption_attempted_ = true;
    }
    const auto work = registry_.pending();
    logger_.write("Inventory=" + std::to_string(inventory.size()) +
                  " pending_objects=" + std::to_string(work.size()));
    stats_.set_state(work.empty() ? "idle" : "fingerprinting");
    std::atomic_size_t next{};
    const auto thread_count =
        (std::min)(work.size(), static_cast<std::size_t>(config_.worker_threads));
    std::jthread heartbeat([&](std::stop_token token) {
      while (!token.stop_requested()) {
        stats_.heartbeat();
        for (int tenth = 0; tenth < 10 && !token.stop_requested(); ++tenth)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t worker_index = 0; worker_index < thread_count;
         ++worker_index) {
      threads.emplace_back([&, worker_index] {
        try {
          fp::Registry registry(config_.database_path);
          fp::ReadOnlyB2Client b2(config_);
          const auto fingerprint_config = this->fingerprint_config();
          gdupe::Fingerprinter fingerprinter(fingerprint_config);
          while (!stopping()) {
            const auto index = next.fetch_add(1);
            if (index >= work.size())
              break;
            process(work[index], worker_index, registry, b2, fingerprinter);
          }
        } catch (const std::exception &problem) {
          logger_.write("Worker thread stopped: " + std::string(problem.what()));
        }
      });
    }
    for (auto &thread : threads)
      thread.join();
    heartbeat.request_stop();
    registry_.set_metadata("currently_processing", "");
    stats_.set_state("idle");
  }

private:
  gdupe::Config fingerprint_config() const {
    gdupe::Config config;
    config.bucket_name = config_.bucket_name;
    config.cache_directory = config_.cache_directory;
    config.fingerprint_version = config_.fingerprint_version;
    config.video_sample_frames = config_.video_sample_frames;
    config.gif_sample_frames = config_.gif_sample_frames;
    return config;
  }

  void process(const fp::PendingObject &item, std::size_t worker_index,
               fp::Registry &registry, fp::ReadOnlyB2Client &b2,
               gdupe::Fingerprinter &fingerprinter) {
    const auto path = cache_path(config_, item.remote);
    stats_.begin(worker_index, item.remote.key);
    logger_.write("Fingerprinting " + item.remote.key + " (" +
                  std::to_string(item.missing_components) +
                  " components missing)");
    bool succeeded = false;
    try {
      const auto download_started = std::chrono::steady_clock::now();
      b2.download_to(item.remote, path,
                     [&](std::uint64_t downloaded, std::uint64_t total) {
                       stats_.progress(worker_index, downloaded, total);
                       return !stopping();
                     });
      stats_.download_finished(worker_index);
      const auto download_seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        download_started)
              .count();
      if (download_seconds > 0.05)
        logger_.write("Downloaded " + item.remote.key + " at " +
                      std::to_string(static_cast<std::uint64_t>(
                          static_cast<double>(item.remote.size) /
                          download_seconds)) +
                      " bytes/s");
      const auto fingerprint =
          fingerprinter.compute(path, item.remote.extension);
      const auto current = b2.find_object(item.remote.key);
      if (!current || !same_identity(*current, item.remote)) {
        logger_.write("Discarded stale result for changed object " +
                      item.remote.key);
      } else {
        registry.save_fingerprint(item.remote, fingerprint);
        succeeded = true;
      }
      std::filesystem::remove(path);
    } catch (const std::exception &problem) {
      std::filesystem::remove(path);
      if (!stopping()) {
        registry.record_failure(item.remote, problem.what(),
                                config_.maximum_item_attempts);
        logger_.write("Item failed without stopping backlog: " +
                      item.remote.key + " -- " + problem.what());
      }
    }
    if (stopping())
      stats_.cancel(worker_index);
    else
      stats_.finish(worker_index, succeeded);
  }

  fp::Config config_;
  Logger &logger_;
  fp::Registry registry_;
  fp::ReadOnlyB2Client b2_;
  fp::RuntimeStats &stats_;
  bool adoption_attempted_{};
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
  const bool gui_mode = arguments.empty();
  if (gui_mode) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (GetConsoleWindow())
      ShowWindow(GetConsoleWindow(), SW_HIDE);
  }
  try {
    const auto executable = executable_path();
    if (has_argument(arguments, "--install-autostart")) {
      install_autostart(executable);
      std::cout << "GParty background fingerprinting will start at logon.\n";
      return 0;
    }
    if (has_argument(arguments, "--remove-autostart")) {
      remove_autostart();
      std::cout << "GParty background fingerprinting autostart was removed.\n";
      return 0;
    }

    const auto config_path = argument_value(arguments, "--config");
    fp::Config config =
        config_path ? fp::Config::load(*config_path)
                    : fp::Config::load_user_or_defaults();

    if (gui_mode) {
      auto credentials = fp::load_credentials();
      bool startup = autostart_installed();
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
          if (!worker_is_running()) {
            start_daemon(executable, config_path);
            wait_for_worker_start();
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
          if (result.autostart) {
            install_autostart(executable);
          } else if (startup) {
            remove_autostart();
          }
          startup = result.autostart;
          if (worker_is_running())
            restart_daemon(executable, config_path);
          else
            start_daemon(executable, config_path);
          fp::show_control_information(
              "Configuration saved. Background fingerprinting is running.\n\n"
              "Use Live CMD Output whenever you want to watch it work.");
        } catch (const std::exception &problem) {
          fp::show_control_error(problem.what());
        }
      }
    }

    if (has_argument(arguments, "--status")) {
      print_status(config);
      return 0;
    }

    const bool restart = has_argument(arguments, "--restart-daemon");
    const bool daemon = has_argument(arguments, "--daemon") || restart;
    if (daemon && GetConsoleWindow())
      ShowWindow(GetConsoleWindow(), SW_HIDE);
    if (restart) {
      for (int attempt = 0; attempt < 18'000 && worker_is_running(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker_is_running())
        throw std::runtime_error("Timed out waiting to restart the worker");
    }

    if (has_argument(arguments, "--viewer")) {
      SetConsoleCtrlHandler(control_handler, TRUE);
      follow_log(config);
      return 0;
    }

    const bool set_credentials = has_argument(arguments, "--set-credentials");
    auto credentials =
        set_credentials ? std::optional<fp::B2Credentials>{}
                        : fp::load_credentials();
    bool newly_entered = false;
    if (!credentials) {
      if (daemon)
        throw std::runtime_error(
            "No read-only B2 login is configured; run --set-credentials once");
      std::cout
          << "GParty Fingerprinter - one-time setup\n\n"
          << "Paste a dedicated Backblaze B2 key restricted to read/list access.\n"
          << "It will be saved in Windows Credential Manager.\n\n"
          << "B2 Key ID: ";
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
      std::cout << "\nChecking the key with Backblaze B2...\n";
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
    fp::RuntimeStats stats(runtime_status_path(config),
                           static_cast<std::size_t>(config.worker_threads));
    Worker worker(config, logger, stats);
    const bool once = has_argument(arguments, "--once");
    int consecutive_failures = 0;
    do {
      try {
        worker.cycle();
        consecutive_failures = 0;
      } catch (const std::exception &problem) {
        ++consecutive_failures;
        logger.write(std::string("Cycle failed: ") + problem.what());
        if (once)
          throw;
      }
      if (once)
        break;
      const int delay = consecutive_failures == 0
                            ? config.polling_seconds
                            : std::min(600, 15 << std::min(5, consecutive_failures - 1));
      for (int second = 0; second < delay && !stopping(); ++second) {
        stats.heartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    } while (!stopping());
    return 0;
  } catch (const std::exception &problem) {
    if (gui_mode)
      fp::show_control_error(problem.what());
    else
      std::cerr << "GParty fingerprinter: " << problem.what() << '\n';
    return 1;
  }
}
