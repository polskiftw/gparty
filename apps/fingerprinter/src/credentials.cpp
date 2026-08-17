#include "credentials.hpp"

#include <windows.h>
#include <wincred.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace gparty::fingerprints {
namespace {

constexpr wchar_t kCredentialTarget[] = L"GParty/fingerprinter-b2";

std::optional<std::string> environment_value(const char *name) {
  char *raw = nullptr;
  std::size_t length = 0;
  const errno_t error = _dupenv_s(&raw, &length, name);
  std::unique_ptr<char, decltype(&std::free)> value(raw, &std::free);
  if (error != 0)
    throw std::runtime_error("Windows could not read the environment");
  if (raw == nullptr || length <= 1)
    return std::nullopt;
  return std::string(raw);
}

std::string wide_to_utf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    throw std::runtime_error("Credential Manager returned invalid text");
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count,
                      nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(std::string_view text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(),
                                        static_cast<int>(text.size()), nullptr,
                                        0);
  if (count <= 0)
    throw std::runtime_error("The B2 key ID is not valid UTF-8");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count);
  return result;
}

struct CredentialDeleter {
  void operator()(CREDENTIALW *credential) const noexcept {
    if (credential)
      CredFree(credential);
  }
};

} // namespace

std::optional<B2Credentials> load_credentials() {
  const auto key_id = environment_value("GPARTY_FP_B2_KEY_ID");
  const auto application_key =
      environment_value("GPARTY_FP_B2_APPLICATION_KEY");
  if (key_id || application_key) {
    if (!key_id || !application_key)
      throw std::runtime_error(
          "GPARTY_FP_B2_KEY_ID and GPARTY_FP_B2_APPLICATION_KEY must be set together");
    return B2Credentials{*key_id, *application_key};
  }

  CREDENTIALW *raw = nullptr;
  if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &raw)) {
    if (GetLastError() == ERROR_NOT_FOUND)
      return std::nullopt;
    throw std::runtime_error(
        "Windows Credential Manager could not read the fingerprinter login");
  }
  std::unique_ptr<CREDENTIALW, CredentialDeleter> credential(raw);
  if (!credential->UserName || !credential->CredentialBlob ||
      credential->CredentialBlobSize == 0)
    throw std::runtime_error("The saved fingerprinter login is incomplete");
  return B2Credentials{
      wide_to_utf8(credential->UserName),
      std::string(reinterpret_cast<const char *>(credential->CredentialBlob),
                  credential->CredentialBlobSize)};
}

void store_credentials(const B2Credentials &credentials) {
  if (credentials.key_id.empty() || credentials.application_key.empty())
    throw std::runtime_error("B2 credentials must not be empty");
  if (credentials.application_key.size() >
      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    throw std::runtime_error("The B2 application key is unexpectedly large");
  std::wstring key_id = utf8_to_wide(credentials.key_id);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<wchar_t *>(kCredentialTarget);
  credential.CredentialBlobSize =
      static_cast<DWORD>(credentials.application_key.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(
      const_cast<char *>(credentials.application_key.data()));
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = key_id.data();
  if (!CredWriteW(&credential, 0))
    throw std::runtime_error(
        "Windows Credential Manager could not save the fingerprinter login");
}

} // namespace gparty::fingerprints
