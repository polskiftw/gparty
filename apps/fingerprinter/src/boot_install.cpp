#include "boot_install.hpp"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <fstream>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace gparty::fingerprints {
namespace {

constexpr wchar_t kTaskName[] = L"GParty Background Fingerprinter";
constexpr wchar_t kWorkerMutex[] =
    L"Global\\GPartyFingerprintRegistryWorker";
constexpr wchar_t kWorkerStopEvent[] =
    L"Global\\GPartyFingerprintRegistryStop";
constexpr wchar_t kInstalledFolder[] = L"GParty";
constexpr wchar_t kInstalledExe[] = L"gparty-fingerprinter.exe";
constexpr wchar_t kMachineConfig[] = L"fingerprinter.json";
constexpr wchar_t kMachineCredentials[] = L"fingerprinter-credentials.bin";
constexpr wchar_t kPayloadName[] = L"boot-install-payload.bin";

std::filesystem::path error_path(const std::filesystem::path &payload) {
  auto result = payload;
  result += L".error";
  return result;
}

struct LocalMemoryDeleter {
  void operator()(void *value) const noexcept {
    if (value)
      LocalFree(value);
  }
};

std::filesystem::path environment_path(const wchar_t *name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required <= 1)
    throw std::runtime_error("Windows installation directories are unavailable");
  std::wstring value(required, L'\0');
  const DWORD written =
      GetEnvironmentVariableW(name, value.data(), required);
  if (written == 0 || written >= required)
    throw std::runtime_error("Windows installation directories are unavailable");
  value.resize(written);
  return std::filesystem::path(value);
}

std::filesystem::path machine_directory() {
  return environment_path(L"ProgramData") / kInstalledFolder;
}

std::filesystem::path installed_directory() {
  return environment_path(L"ProgramFiles") / kInstalledFolder;
}

std::filesystem::path installed_executable() {
  return installed_directory() / kInstalledExe;
}

std::filesystem::path machine_credentials_path() {
  return machine_directory() / kMachineCredentials;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::runtime_error("Cannot read the boot installation payload");
  const auto end = stream.tellg();
  if (end <= 0 || end > 16 * 1024 * 1024)
    throw std::runtime_error("Boot installation payload has an invalid size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream)
    throw std::runtime_error("Cannot finish reading the boot installation payload");
  return bytes;
}

void write_bytes(const std::filesystem::path &path,
                 const std::vector<std::uint8_t> &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    throw std::runtime_error("Cannot write the protected boot configuration");
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream)
    throw std::runtime_error("Cannot finish the protected boot configuration");
}

std::vector<std::uint8_t> protect_machine(std::string_view plaintext) {
  if (plaintext.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    throw std::runtime_error("Boot configuration is unexpectedly large");
  DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.data()))};
  DATA_BLOB output{};
  if (!CryptProtectData(
          &input, L"GParty boot worker", nullptr, nullptr, nullptr,
          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("Windows could not protect the boot configuration");
  std::unique_ptr<void, LocalMemoryDeleter> owner(output.pbData);
  return {output.pbData, output.pbData + output.cbData};
}

std::string unprotect_machine(const std::vector<std::uint8_t> &ciphertext) {
  if (ciphertext.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    throw std::runtime_error("Protected boot configuration is unexpectedly large");
  DATA_BLOB input{static_cast<DWORD>(ciphertext.size()),
                  const_cast<BYTE *>(ciphertext.data())};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("Windows could not unlock the boot configuration");
  std::unique_ptr<void, LocalMemoryDeleter> owner(output.pbData);
  return {reinterpret_cast<const char *>(output.pbData), output.cbData};
}

void restrict_to_system_and_administrators(const std::filesystem::path &path) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &raw, nullptr))
    throw std::runtime_error("Windows could not create the boot security policy");
  std::unique_ptr<void, LocalMemoryDeleter> descriptor(raw);
  if (!SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                        static_cast<PSECURITY_DESCRIPTOR>(descriptor.get())))
    throw std::runtime_error("Windows could not protect the boot configuration file");
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
    throw std::runtime_error("Windows could not start Task Scheduler");
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

void stop_boot_worker() {
  const HANDLE event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, kWorkerStopEvent);
  if (event) {
    SetEvent(event);
    CloseHandle(event);
  }
  for (int attempt = 0; attempt < 300; ++attempt) {
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kWorkerMutex);
    if (!mutex)
      return;
    CloseHandle(mutex);
    Sleep(100);
  }
  run_process({L"schtasks.exe", L"/End", L"/TN", kTaskName});
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Cannot read the saved GUI configuration");
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    throw std::runtime_error("Cannot write the machine boot configuration");
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream)
    throw std::runtime_error("Cannot finish the machine boot configuration");
}

} // namespace

std::filesystem::path machine_config_path() {
  return machine_directory() / kMachineConfig;
}

std::filesystem::path create_boot_install_payload(
    const Config &, const B2Credentials &credentials) {
  const nlohmann::json payload{
      {"config_json", read_text(user_config_path())},
      {"key_id", credentials.key_id},
      {"application_key", credentials.application_key}};
  const auto path = user_config_path().parent_path() / kPayloadName;
  write_bytes(path, protect_machine(payload.dump()));
  return path;
}

B2Credentials load_machine_credentials() {
  const auto plaintext =
      unprotect_machine(read_bytes(machine_credentials_path()));
  const auto value = nlohmann::json::parse(plaintext);
  return {value.at("key_id").get<std::string>(),
          value.at("application_key").get<std::string>()};
}

bool boot_worker_installed() {
  return run_process({L"schtasks.exe", L"/Query", L"/TN", kTaskName}) == 0;
}

void install_boot_worker(const std::filesystem::path &source_executable,
                         const std::filesystem::path &payload_path) {
  const auto payload_plaintext = unprotect_machine(read_bytes(payload_path));
  const auto payload = nlohmann::json::parse(payload_plaintext);
  const B2Credentials credentials{
      payload.at("key_id").get<std::string>(),
      payload.at("application_key").get<std::string>()};
  if (credentials.key_id.empty() || credentials.application_key.empty())
    throw std::runtime_error("The boot worker credentials are incomplete");

  stop_boot_worker();
  run_process({L"schtasks.exe", L"/Delete", L"/F", L"/TN", kTaskName});
  std::filesystem::create_directories(installed_directory());
  const auto installed = installed_executable();
  if (std::filesystem::weakly_canonical(source_executable) !=
      std::filesystem::weakly_canonical(installed))
    std::filesystem::copy_file(source_executable, installed,
                               std::filesystem::copy_options::overwrite_existing);

  write_text(machine_config_path(),
             payload.at("config_json").get<std::string>());
  restrict_to_system_and_administrators(machine_config_path());
  const nlohmann::json credential_json{
      {"key_id", credentials.key_id},
      {"application_key", credentials.application_key}};
  write_bytes(machine_credentials_path(),
              protect_machine(credential_json.dump()));
  restrict_to_system_and_administrators(machine_credentials_path());

  const std::wstring task_action =
      L"\"" + installed.wstring() + L"\" --boot-worker";
  const int created = run_process(
      {L"schtasks.exe", L"/Create", L"/F", L"/SC", L"ONSTART", L"/RU",
       L"SYSTEM", L"/RL", L"HIGHEST", L"/TN", kTaskName, L"/TR",
       task_action});
  if (created != 0)
    throw std::runtime_error("Task Scheduler rejected the boot worker");
  if (run_process({L"schtasks.exe", L"/Run", L"/TN", kTaskName}) != 0)
    throw std::runtime_error("The boot worker was installed but could not start");
  std::error_code ignored;
  std::filesystem::remove(payload_path, ignored);
}

void report_boot_install_error(const std::filesystem::path &payload_path,
                               const std::string &message) noexcept {
  try {
    write_text(error_path(payload_path), message);
  } catch (...) {
  }
}

void uninstall_boot_worker() {
  stop_boot_worker();
  if (run_process({L"schtasks.exe", L"/Delete", L"/F", L"/TN", kTaskName}) !=
      0)
    throw std::runtime_error("Task Scheduler could not remove the boot worker");
}

void run_elevated_boot_action(const std::filesystem::path &executable,
                              const std::wstring &action,
                              const std::filesystem::path &payload) {
  std::vector<std::wstring> arguments{action};
  if (!payload.empty())
    arguments.push_back(payload.wstring());
  const auto child_error = error_path(payload);
  std::error_code ignored;
  if (!payload.empty())
    std::filesystem::remove(child_error, ignored);
  const std::wstring parameters = command_line(arguments);
  SHELLEXECUTEINFOW launch{};
  launch.cbSize = sizeof(launch);
  launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  launch.lpVerb = L"runas";
  launch.lpFile = executable.c_str();
  launch.lpParameters = parameters.c_str();
  launch.nShow = SW_HIDE;
  if (!ShellExecuteExW(&launch)) {
    const DWORD shell_error = GetLastError();
    if (!payload.empty())
      std::filesystem::remove(payload, ignored);
    if (shell_error == ERROR_CANCELLED)
      throw std::runtime_error(
          "Boot installation was cancelled in the Windows permission prompt");
    throw std::runtime_error("Windows could not open the boot installer");
  }
  WaitForSingleObject(launch.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(launch.hProcess, &exit_code);
  CloseHandle(launch.hProcess);
  std::string detail;
  if (!payload.empty() && std::filesystem::exists(child_error, ignored)) {
    try {
      detail = read_text(child_error);
    } catch (...) {
    }
  }
  if (!payload.empty()) {
    std::filesystem::remove(payload, ignored);
    std::filesystem::remove(child_error, ignored);
  }
  if (exit_code != 0) {
    if (!detail.empty())
      throw std::runtime_error(detail);
    throw std::runtime_error("The elevated boot installation did not complete");
  }
}

} // namespace gparty::fingerprints
