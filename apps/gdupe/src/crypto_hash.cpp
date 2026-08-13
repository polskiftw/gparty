#include "crypto_hash.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#error gdupe's supported cryptographic backend is Windows CNG
#endif

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace gdupe {
namespace {

class Hash {
public:
  explicit Hash(const wchar_t *algorithm) {
    try {
      require(BCryptOpenAlgorithmProvider(&algorithm_, algorithm, nullptr, 0),
              "open hash provider");
      DWORD bytes = 0;
      DWORD returned = 0;
      require(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes),
                                &returned, 0),
              "read hash object size");
      object_.resize(bytes);
      require(BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes),
                                &returned, 0),
              "read digest size");
      digest_.resize(bytes);
      require(BCryptCreateHash(
                  algorithm_, &hash_, object_.data(),
                  static_cast<ULONG>(object_.size()), nullptr, 0, 0),
              "create hash");
    } catch (...) {
      if (hash_)
        BCryptDestroyHash(hash_);
      if (algorithm_)
        BCryptCloseAlgorithmProvider(algorithm_, 0);
      throw;
    }
  }

  ~Hash() {
    if (hash_)
      BCryptDestroyHash(hash_);
    if (algorithm_)
      BCryptCloseAlgorithmProvider(algorithm_, 0);
  }

  Hash(const Hash &) = delete;
  Hash &operator=(const Hash &) = delete;

  void update(const void *data, std::size_t size) {
    if (size == 0)
      return;
    if (size > std::numeric_limits<ULONG>::max())
      throw std::runtime_error("Hash input chunk is too large");
    require(BCryptHashData(
                hash_, const_cast<PUCHAR>(
                           reinterpret_cast<const unsigned char *>(data)),
                static_cast<ULONG>(size), 0),
            "update hash");
  }

  std::string finish() {
    require(BCryptFinishHash(hash_, digest_.data(),
                             static_cast<ULONG>(digest_.size()), 0),
            "finish hash");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest_)
      output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
  }

private:
  BCRYPT_ALG_HANDLE algorithm_{};
  BCRYPT_HASH_HANDLE hash_{};
  std::vector<unsigned char> object_;
  std::vector<unsigned char> digest_;

  static void require(NTSTATUS status, const char *operation) {
    if (!BCRYPT_SUCCESS(status))
      throw std::runtime_error(std::string("Windows CNG could not ") +
                               operation);
  }
};

std::string digest(std::string_view value, const wchar_t *algorithm) {
  Hash hash(algorithm);
  hash.update(value.data(), value.size());
  return hash.finish();
}

std::string digest_file(const std::filesystem::path &path,
                        const wchar_t *algorithm) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot open file for hashing");
  Hash hash(algorithm);
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0)
      hash.update(buffer.data(), static_cast<std::size_t>(count));
  }
  if (input.bad())
    throw std::runtime_error("Cannot read file while hashing");
  return hash.finish();
}

} // namespace

std::string sha1(std::string_view value) {
  return digest(value, BCRYPT_SHA1_ALGORITHM);
}

std::string sha256(std::string_view value) {
  return digest(value, BCRYPT_SHA256_ALGORITHM);
}

std::string sha1_file(const std::filesystem::path &path) {
  return digest_file(path, BCRYPT_SHA1_ALGORITHM);
}

std::string sha256_file(const std::filesystem::path &path) {
  return digest_file(path, BCRYPT_SHA256_ALGORITHM);
}

} // namespace gdupe
