#pragma once

#include "config.hpp"
#include "model.hpp"

#include <filesystem>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace gdupe {

class B2Client {
public:
  explicit B2Client(const Config &config);
  ~B2Client();

  void enable_persistent_download_connection();

  std::vector<RemoteObject> list_objects(const std::string &prefix);
  std::vector<RemoteObject> stable_inventory(const std::string &prefix);
  std::optional<RemoteObject> find_object(const std::string &key);
  bool version_exists(const std::string &key, const std::string &file_id);
  void download_to(const RemoteObject &object,
                   const std::filesystem::path &destination);
  std::string download_bytes(const RemoteObject &object);
  void delete_version(const std::string &key, const std::string &file_id);
  RemoteObject
  upload_bytes(const std::string &key, const std::string &payload,
               const std::string &content_type,
               const std::map<std::string, std::string> &metadata = {});
  void prune_old_versions(const std::string &key,
                          const std::string &keep_file_id);

  std::string inventory_digest(const std::vector<RemoteObject> &objects) const;
  std::string
  canonical_index_payload(const std::vector<RemoteObject> &objects) const;
  std::vector<RemoteObject>
  settle_canonical_index(const std::vector<RemoteObject> &initial);

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
  std::string upload_url_;
  std::string upload_token_;
  void *persistent_download_handle_{};

  const Authorization &authorize(bool force = false);
  nlohmann::json api(const std::string &method, const nlohmann::json &body,
                     const std::string &operation);
  Response request(const std::string &method, const std::string &url,
                   const std::vector<std::string> &headers,
                   const std::string &body = {},
                   const std::string &user_password = {});
  void require_capabilities(std::initializer_list<const char *> capabilities);
  void ensure_bucket_id();
  void ensure_upload_endpoint();
};

} // namespace gdupe
