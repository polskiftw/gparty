#include "nvdec_decode.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
    return 77;
  }
  if (argc != 2) {
    std::cerr << "NVDEC test gate requires exactly one test executable path\n";
    return 2;
  }

  const std::string command = std::string("\"") + argv[1] + "\"";
  return std::system(command.c_str());
}
