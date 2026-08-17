#pragma once

#include "config.hpp"
#include "model.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace gparty::fingerprints {

class ReadOnlyB2Client {
public:
  using DownloadProgress =
      std::function<bool(std::uint64_t downloaded, std::uint64_t total)>;
  explicit ReadOnlyB2Client(const Config &config);

  std::vector<gdupe::RemoteObject> list_objects(const std::string &prefix);
  std::optional<gdupe::RemoteObject> find_object(const std::string &key);
  void download_to(const gdupe::RemoteObject &object,
                   const std::filesystem::path &destination,
                   DownloadProgress progress = {});

private:
  struct Authorization {
    std::string token;
    std::string api_url;
    std::string download_url;
    std::string account_id;
    std::string bucket_id;
    std::set<std::string> capabilities;
  };
  struct Response {
    long status{};
    int curl_code{};
    std::string body;
  };

  Config config_;
  Authorization authorization_;
  std::mutex authorization_mutex_;

  const Authorization &authorize(bool force = false);
  nlohmann::json api(const std::string &method, const nlohmann::json &body,
                     const std::string &operation);
  Response request(const std::string &method, const std::string &url,
                   const std::vector<std::string> &headers,
                   const std::string &body = {},
                   const std::string &user_password = {});
  void require_capabilities(std::initializer_list<const char *> capabilities);
  void ensure_bucket_id();
};

} // namespace gparty::fingerprints
