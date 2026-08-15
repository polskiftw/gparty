#include "image_decode.hpp"
#include "media_test_util.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using gdupe_test::TempMedia;
using gdupe_test::require;

void test_malformed_jpeg_fails_cleanly() {
  TempMedia invalid("jpg");
  for (int attempt = 0; attempt < 32; ++attempt) {
    bool rejected = false;
    try {
      (void)gdupe::decode_static_image(invalid.path(), "jpg");
    } catch (const std::runtime_error &problem) {
      const std::string message = problem.what();
      require(message.find("JPEG decoder rejected the image") !=
                  std::string::npos,
              "malformed JPEG returned an unexpected error");
      rejected = true;
    }
    require(rejected, "malformed JPEG was unexpectedly accepted");
  }
}

} // namespace

int main() {
  try {
    test_malformed_jpeg_fails_cleanly();
    std::cout << "JPEG failure-path regression passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "JPEG failure-path regression failed: " << problem.what()
              << '\n';
    return 1;
  }
}
