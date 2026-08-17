#include "config.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "fingerprint.hpp"
#include "readonly_b2.hpp"
#include "registry.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fp = gparty::fingerprints;

namespace {

constexpr wchar_t kMutexName[] = L"Local\\GPartyFingerprintRegistryWorker";
constexpr wchar_t kTaskName[] = L"GParty Background Fingerprinter";
std::atomic_bool stop_requested{};

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
  explicit Logger(std::filesystem::path path) : path_(std::move(path)) {
    if (!path_.parent_path().empty())
      std::filesystem::create_directories(path_.parent_path());
    rotate();
  }

  void write(const std::string &message) {
    std::scoped_lock lock(mutex_);
    std::ofstream stream(path_, std::ios::app);
    if (!stream)
      return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    stream << time.wYear << '-';
    stream.width(2);
    stream.fill('0');
    stream << time.wMonth << '-';
    stream.width(2);
    stream << time.wDay << ' ';
    stream.width(2);
    stream << time.wHour << ':';
    stream.width(2);
    stream << time.wMinute << ':';
    stream.width(2);
    stream << time.wSecond << "  " << message << '\n';
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

int run_process(const std::vector<std::wstring> &arguments) {
  std::wstring command;
  for (const auto &argument : arguments) {
    if (!command.empty())
      command.push_back(L' ');
    command += quote_argument(argument);
  }
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

class Worker {
public:
  Worker(fp::Config config, Logger &logger)
      : config_(std::move(config)), logger_(logger), registry_(config_.database_path),
        b2_(config_), fingerprint_config_storage_(fingerprint_config()),
        fingerprinter_(fingerprint_config_storage_) {
    clean_partials(config_.cache_directory);
  }

  void cycle() {
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
    for (const auto &item : work) {
      if (stop_requested)
        break;
      process(item);
    }
    registry_.set_metadata("currently_processing", "");
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

  void process(const fp::PendingObject &item) {
    const auto path = cache_path(config_, item.remote);
    registry_.set_metadata("currently_processing", item.remote.key);
    logger_.write("Fingerprinting " + item.remote.key + " (" +
                  std::to_string(item.missing_components) +
                  " components missing)");
    try {
      b2_.download_to(item.remote, path);
      const auto fingerprint =
          fingerprinter_.compute(path, item.remote.extension);
      const auto current = b2_.find_object(item.remote.key);
      if (!current || !same_identity(*current, item.remote)) {
        logger_.write("Discarded stale result for changed object " +
                      item.remote.key);
      } else {
        registry_.save_fingerprint(item.remote, fingerprint);
      }
      std::filesystem::remove(path);
    } catch (const std::exception &problem) {
      std::filesystem::remove(path);
      registry_.record_failure(item.remote, problem.what(),
                               config_.maximum_item_attempts);
      logger_.write("Item failed without stopping backlog: " + item.remote.key +
                    " -- " + problem.what());
    }
  }

  fp::Config config_;
  Logger &logger_;
  fp::Registry registry_;
  fp::ReadOnlyB2Client b2_;
  gdupe::Config fingerprint_config_storage_{};
  gdupe::Fingerprinter fingerprinter_;
  bool adoption_attempted_{};
};

BOOL WINAPI control_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
    stop_requested = true;
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
  try {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
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

    const auto config_path =
        argument_value(arguments, "--config")
            .value_or(fp::default_config_path(executable));
    fp::Config config = fp::Config::load(config_path);
    if (has_argument(arguments, "--status")) {
      print_status(config);
      return 0;
    }

    const bool daemon = has_argument(arguments, "--daemon");
    if (daemon && GetConsoleWindow())
      ShowWindow(GetConsoleWindow(), SW_HIDE);

    const bool set_credentials = has_argument(arguments, "--set-credentials");
    auto credentials =
        set_credentials ? std::optional<fp::B2Credentials>{}
                        : fp::load_credentials();
    bool newly_entered = false;
    if (!credentials) {
      if (daemon)
        throw std::runtime_error(
            "No read-only B2 login is configured; run --set-credentials once");
      std::cout << "B2 Key ID: ";
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
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler(control_handler, TRUE);
    Logger logger(config.log_path);
    Worker worker(config, logger);
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
      for (int second = 0; second < delay && !stop_requested; ++second)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (!stop_requested);
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "GParty fingerprinter: " << problem.what() << '\n';
    return 1;
  }
}
