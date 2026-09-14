#include "credentials.hpp"
#include "win32_util.hpp"

#include <windows.h>
#include <wincred.h>

#include <limits>
#include <memory>
#include <stdexcept>

namespace gparty::fingerprints {
namespace {

constexpr wchar_t kCredentialTarget[] = L"GParty/gfingerd-b2";

struct CredentialDeleter {
  void operator()(CREDENTIALW *credential) const noexcept {
    if (credential)
      CredFree(credential);
  }
};

} // namespace

std::optional<B2Credentials> load_credentials() {
  CREDENTIALW *raw = nullptr;
  if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &raw)) {
    if (GetLastError() == ERROR_NOT_FOUND)
      return std::nullopt;
    throw std::runtime_error(
        "Windows Credential Manager could not read the gfingerd login");
  }
  std::unique_ptr<CREDENTIALW, CredentialDeleter> credential(raw);
  if (!credential->UserName || !credential->CredentialBlob ||
      credential->CredentialBlobSize == 0)
    throw std::runtime_error("The saved gfingerd login is incomplete");
  return B2Credentials{
      win32::wide_to_utf8(credential->UserName),
      std::string(reinterpret_cast<const char *>(credential->CredentialBlob),
                  credential->CredentialBlobSize)};
}

void store_credentials(const B2Credentials &credentials) {
  if (credentials.key_id.empty() || credentials.application_key.empty())
    throw std::runtime_error("B2 credentials must not be empty");
  if (credentials.application_key.size() >
      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
    throw std::runtime_error("The B2 application key is unexpectedly large");
  std::wstring key_id = win32::utf8_to_wide(credentials.key_id);
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
        "Windows Credential Manager could not save the gfingerd login");
}

} // namespace gparty::fingerprints
