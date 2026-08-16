#include "b2_client.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "fingerprint.hpp"
#include "wic_gif.hpp"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

struct Options {
  std::optional<std::filesystem::path> config;
  std::optional<std::filesystem::path> report;
};

std::filesystem::path executable_path() {
  std::vector<wchar_t> buffer(1024);
  while (true) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0)
      throw std::runtime_error("Windows could not locate gdupe-gif-sweep.exe");
    if (length < buffer.size() - 1)
      return std::filesystem::path(std::wstring(buffer.data(), length));
    if (buffer.size() >= 32768)
      throw std::runtime_error("The GIF sweep executable path is too long");
    buffer.resize(buffer.size() * 2);
  }
}

Options parse_options(int argc, wchar_t **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--config" || argument == L"--report") {
      if (index + 1 >= argc)
        throw std::runtime_error("A command-line option is missing its path");
      const std::filesystem::path value(argv[++index]);
      if (argument == L"--config")
        options.config = value;
      else
        options.report = value;
      continue;
    }
    if (argument == L"--help" || argument == L"-h") {
      std::cout << "gdupe-gif-sweep [--config PATH] [--report PATH]\n";
      std::exit(0);
    }
    throw std::runtime_error("Unknown command-line option");
  }
  return options;
}

nlohmann::json normalization_json(
    const gdupe::WicGifNormalization &normalization) {
  return {{"frame", normalization.frame_index},
          {"left", normalization.left},
          {"top", normalization.top},
          {"declared_width", normalization.declared_width},
          {"declared_height", normalization.declared_height},
          {"decoded_width", normalization.decoded_width},
          {"decoded_height", normalization.decoded_height},
          {"expanded_canvas", normalization.expanded_canvas},
          {"size_metadata_mismatch",
           normalization.size_metadata_mismatch}};
}

nlohmann::json base_record(const gdupe::RemoteObject &object) {
  return {{"type", "gif"},
          {"key", object.key},
          {"file_id", object.file_id},
          {"size", object.size}};
}

void append_info(nlohmann::json &record, const gdupe::WicGifInfo &info) {
  record["logical_width"] = info.logical_width;
  record["logical_height"] = info.logical_height;
  record["effective_width"] = info.width;
  record["effective_height"] = info.height;
  record["frame_count"] = info.frame_count;
  record["normalizations"] = nlohmann::json::array();
  for (const auto &normalization : info.normalizations)
    record["normalizations"].push_back(normalization_json(normalization));
}

void write_record(std::ofstream &report, const nlohmann::json &record) {
  report << record.dump() << '\n';
  report.flush();
  if (!report)
    throw std::runtime_error("Could not write the GIF sweep report");
}

void remove_local_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error)
    throw std::runtime_error("Could not remove a temporary GIF sweep file: " +
                             error.message());
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    const Options options = parse_options(argc, argv);
    const auto executable = executable_path();
    const auto config_path =
        options.config.value_or(gdupe::default_config_path(executable));
    const auto report_path = options.report.value_or(
        executable.parent_path() / "gdupe-gif-sweep-report.jsonl");

    auto config = gdupe::Config::load(config_path);
    const auto credentials = gdupe::load_b2_credentials();
    if (!credentials) {
      throw std::runtime_error(
          "No saved B2 credential was found. Run gdupe once first so the "
          "validated credential exists in Windows Credential Manager.");
    }
    config.key_id = credentials->key_id;
    config.application_key = credentials->application_key;

    std::ofstream report(report_path, std::ios::trunc);
    if (!report)
      throw std::runtime_error("Could not create the GIF sweep report");

    std::cout << "Listing B2 GIF objects...\n";
    gdupe::B2Client client(config);
    const auto inventory = client.list_objects(config.canonical_prefix);
    std::vector<gdupe::RemoteObject> gifs;
    for (const auto &object : inventory) {
      if (object.extension == "gif")
        gifs.push_back(object);
    }

    write_record(report,
                 {{"type", "start"},
                  {"gif_count", gifs.size()},
                  {"prefix", config.canonical_prefix}});
    std::cout << "Found " << gifs.size()
              << " GIFs. This sweep is read-only against B2.\n";

    const auto temp_root =
        std::filesystem::temp_directory_path() / "gdupe-gif-sweep";
    std::filesystem::create_directories(temp_root);
    gdupe::Fingerprinter fingerprinter(config);

    std::size_t ok = 0;
    std::size_t normalized = 0;
    std::size_t failed = 0;
    for (std::size_t index = 0; index < gifs.size(); ++index) {
      const auto &object = gifs[index];
      const auto local =
          temp_root / (gdupe::sha256(object.file_id).substr(0, 32) + ".gif");
      std::optional<gdupe::WicGifInfo> inspected;
      std::optional<std::string> failure;

      try {
        client.download_to(object, local);
        {
          gdupe::WicGifDecoder decoder(local);
          inspected = decoder.info();
        }
        (void)fingerprinter.compute(local, "gif");
      } catch (const std::exception &problem) {
        failure = problem.what();
      }

      if (std::filesystem::exists(local))
        remove_local_file(local);

      if (failure) {
        ++failed;
        auto record = base_record(object);
        record["status"] = "error";
        record["error"] = *failure;
        if (inspected)
          append_info(record, *inspected);
        write_record(report, record);
        std::cout << "ERROR " << (index + 1) << '/' << gifs.size() << ": "
                  << object.key << " -> " << *failure << '\n';
      } else if (inspected && !inspected->normalizations.empty()) {
        ++normalized;
        auto record = base_record(object);
        record["status"] = "normalized";
        append_info(record, *inspected);
        write_record(report, record);
        std::cout << "NORMALIZED " << (index + 1) << '/' << gifs.size()
                  << ": " << object.key << " ("
                  << inspected->normalizations.size()
                  << " frame geometry adjustment(s))\n";
      } else {
        ++ok;
      }

      if ((index + 1) % 100 == 0 || index + 1 == gifs.size()) {
        std::cout << "Progress: " << (index + 1) << '/' << gifs.size()
                  << " | clean=" << ok << " normalized=" << normalized
                  << " errors=" << failed << '\n';
      }
    }

    write_record(report,
                 {{"type", "summary"},
                  {"total", gifs.size()},
                  {"clean", ok},
                  {"normalized", normalized},
                  {"errors", failed}});
    std::error_code remove_error;
    std::filesystem::remove(temp_root, remove_error);

    std::cout << "\nDone. Report: " << report_path.string() << '\n'
              << "Clean: " << ok << " | Normalized: " << normalized
              << " | Errors: " << failed << '\n';
    return failed == 0 ? 0 : 2;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe GIF sweep failed: " << problem.what() << '\n';
    return 1;
  }
}
