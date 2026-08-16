#pragma once

#include <optional>
#include <string>

namespace gdupe {

struct B2Credentials {
  std::string key_id;
  std::string application_key;
};

std::optional<B2Credentials> load_b2_credentials();
void store_b2_credentials(const B2Credentials &credentials);

} // namespace gdupe
