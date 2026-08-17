#pragma once

#include <optional>
#include <string>

namespace gparty::fingerprints {

struct B2Credentials {
  std::string key_id;
  std::string application_key;
};

std::optional<B2Credentials> load_credentials();
void store_credentials(const B2Credentials &credentials);

} // namespace gparty::fingerprints
