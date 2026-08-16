#include "b2_client.hpp"
#include "config.hpp"
#include "credentials.hpp"
#include "crypto_hash.hpp"
#include "database.hpp"
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

constexpr wchar_t kGdupeInstanceMutex[] = L"Local\\gdupe-single-instance-v1";
constexpr const char *kBrokenDirectory = "broken-originals";
constexpr const char *kNormalizedDirectory = "normalized-originals";

struct Options {
  std::optional<std::filesystem::path> config;
  std::optional<std::filesystem::path> report;
};

class GdupeProcessLock final {
public:
  GdupeProcessLock() {
    handle_ = CreateMutexW(nullptr, FALSE, kGdupeInstanceMutex);
    if (!handle_)
      throw std::runtime_error("Windows could not create the gdupe instance lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error(
          "Close gdupe before running the GIF sweep; the sweep writes clean "
          "fingerprints into gdupe's local database");
    }
  }

  ~GdupeProcessLock() {
    if (handle_)
      CloseHandle(handle_);
  }

  GdupeProcessLock(const GdupeProcessLock &) = delete;
  GdupeProcessLock &operator=(const GdupeProcessLock &) = delete;

private:
  HANDLE handle_{};
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

void remove_tool_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error)
    throw std::runtime_error("Could not remove a GIF sweep staging file: " +
                             error.message());
}

std::filesystem::path stable_gif_name(const gdupe::RemoteObject &object) {
  return gdupe::sha256(object.file_id).substr(0, 32) + ".gif";
}

bool exact_local_copy(const std::filesystem::path &path,
                      const gdupe::RemoteObject &object) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    return false;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size != object.size)
    return false;
  return object.sha1.size() == 40 && gdupe::sha1_file(path) == object.sha1;
}

void require_finding_integrity(const std::filesystem::path &path,
                               const gdupe::RemoteObject &object) {
  if (!exact_local_copy(path, object)) {
    throw std::runtime_error(
        "A saved GIF finding no longer matches its B2 object: " +
        path.string());
  }
}

std::optional<std::filesystem::path>
existing_finding(const std::filesystem::path &findings_root,
                 const gdupe::RemoteObject &object) {
  for (const char *directory : {kNormalizedDirectory, kBrokenDirectory}) {
    const auto candidate = findings_root / directory / stable_gif_name(object);
    if (std::filesystem::exists(candidate)) {
      require_finding_integrity(candidate, object);
      return candidate;
    }
  }
  return std::nullopt;
}

std::filesystem::path
preserve_finding(const std::filesystem::path &source,
                 const std::filesystem::path &findings_root,
                 const char *directory, const gdupe::RemoteObject &object,
                 bool remove_source) {
  const auto destination =
      findings_root / directory / stable_gif_name(object);
  if (source == destination) {
    require_finding_integrity(destination, object);
    return destination;
  }
  if (std::filesystem::exists(destination)) {
    require_finding_integrity(destination, object);
    if (remove_source)
      remove_tool_file(source);
    return destination;
  }

  std::filesystem::create_directories(destination.parent_path());
  auto partial = destination;
  partial += ".partial";
  std::error_code cleanup_error;
  std::filesystem::remove(partial, cleanup_error);
  std::filesystem::copy_file(source, partial);
  if (!exact_local_copy(partial, object)) {
    std::filesystem::remove(partial, cleanup_error);
    throw std::runtime_error(
        "A GIF finding failed integrity verification while being preserved");
  }
  std::filesystem::rename(partial, destination);
  if (remove_source)
    remove_tool_file(source);
  return destination;
}

bool has_current_fingerprint(const gdupe::InventoryObject &item,
                             int fingerprint_version) {
  return item.fingerprint && item.fingerprint->version == fingerprint_version;
}

void verify_saved_fingerprint(const gdupe::Database &database,
                              const gdupe::RemoteObject &object,
                              const gdupe::Fingerprint &fingerprint) {
  const auto saved = database.object(object.key);
  if (!saved || saved->remote.file_id != object.file_id ||
      !saved->fingerprint ||
      saved->fingerprint->version != fingerprint.version ||
      saved->fingerprint->sha256 != fingerprint.sha256) {
    throw std::runtime_error(
        "Clean GIF fingerprint did not verify after the local database write");
  }
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    GdupeProcessLock process_lock;
    const Options options = parse_options(argc, argv);
    const auto executable = executable_path();
    const auto config_path =
        options.config.value_or(gdupe::default_config_path(executable));
    const auto report_path = options.report.value_or(
        executable.parent_path() / "gdupe-gif-sweep-report.jsonl");
    const auto findings_root =
        executable.parent_path() / "gif-sweep-findings";

    auto config = gdupe::Config::load(config_path);
    if (!std::filesystem::exists(config.database_path)) {
      throw std::runtime_error(
          "The local gdupe inventory database is missing. Run gdupe through "
          "a successful B2 synchronization before starting the GIF sweep.");
    }

    gdupe::Database database(config.database_path);
    const std::string inventory_digest =
        database.metadata("last_inventory_sha256");
    if (inventory_digest.empty()) {
      throw std::runtime_error(
          "The local gdupe database has no completed inventory marker. Run "
          "gdupe through a successful B2 synchronization first.");
    }

    const auto local_inventory = database.inventory();
    std::vector<gdupe::InventoryObject> gifs;
    gifs.reserve(local_inventory.size());
    std::size_t already_fingerprinted = 0;
    for (const auto &item : local_inventory) {
      if (item.remote.extension != "gif")
        continue;
      if (has_current_fingerprint(item, config.fingerprint_version)) {
        ++already_fingerprinted;
        continue;
      }
      gifs.push_back(item);
    }

    const auto credentials = gdupe::load_b2_credentials();
    if (!credentials && !gifs.empty()) {
      throw std::runtime_error(
          "No saved B2 credential was found. Run gdupe once first so the "
          "validated credential exists in Windows Credential Manager.");
    }
    if (credentials) {
      config.key_id = credentials->key_id;
      config.application_key = credentials->application_key;
    }

    std::filesystem::create_directories(findings_root / kBrokenDirectory);
    std::filesystem::create_directories(findings_root / kNormalizedDirectory);
    std::ofstream report(report_path, std::ios::trunc);
    if (!report)
      throw std::runtime_error("Could not create the GIF sweep report");

    write_record(report,
                 {{"type", "start"},
                  {"inventory_source", "local_gdupe_database"},
                  {"inventory_digest", inventory_digest},
                  {"database_path", config.database_path.string()},
                  {"gif_count", gifs.size() + already_fingerprinted},
                  {"already_fingerprinted", already_fingerprinted},
                  {"to_sweep", gifs.size()},
                  {"findings_root", findings_root.string()}});

    std::cout << "Using gdupe's completed local inventory.\n"
              << "GIFs: " << (gifs.size() + already_fingerprinted)
              << " | already fingerprinted: " << already_fingerprinted
              << " | to sweep: " << gifs.size() << "\n"
              << "B2 remains read-only. Clean fingerprints are committed "
                 "safely to the local gdupe database.\n"
              << "Normalized/error GIFs are kept only as source files under: "
              << findings_root.string() << "\n\n";

    const auto temp_root =
        std::filesystem::temp_directory_path() / "gdupe-gif-sweep";
    std::filesystem::create_directories(temp_root);
    gdupe::Fingerprinter fingerprinter(config);
    std::optional<gdupe::B2Client> client;
    if (!gifs.empty())
      client.emplace(config);

    std::size_t clean_saved = 0;
    std::size_t clean_already_present = 0;
    std::size_t normalized = 0;
    std::size_t failed = 0;
    std::size_t reused_saved_source = 0;

    for (std::size_t index = 0; index < gifs.size(); ++index) {
      const auto &item = gifs[index];
      const auto &object = item.remote;
      const auto temporary = temp_root / stable_gif_name(object);
      std::optional<std::filesystem::path> source;
      bool source_is_temporary = false;
      std::optional<gdupe::WicGifInfo> inspected;
      std::optional<gdupe::Fingerprint> fingerprint;
      std::optional<std::string> failure;

      try {
        if (auto finding = existing_finding(findings_root, object)) {
          source = *finding;
          ++reused_saved_source;
        } else if (std::filesystem::exists(temporary) &&
                   exact_local_copy(temporary, object)) {
          source = temporary;
          source_is_temporary = true;
        } else {
          if (std::filesystem::exists(temporary))
            remove_tool_file(temporary);
          client->download_to(object, temporary);
          source = temporary;
          source_is_temporary = true;
        }

        {
          gdupe::WicGifDecoder decoder(*source);
          inspected = decoder.info();
        }
        fingerprint = fingerprinter.compute(*source, "gif");
      } catch (const std::exception &problem) {
        failure = problem.what();
      }

      if (failure) {
        ++failed;
        auto record = base_record(object);
        record["status"] = "error";
        record["error"] = *failure;
        if (inspected)
          append_info(record, *inspected);
        if (source && std::filesystem::exists(*source)) {
          const auto saved = preserve_finding(
              *source, findings_root, kBrokenDirectory, object,
              source_is_temporary);
          record["saved_original"] = saved.string();
        }
        write_record(report, record);
        std::cout << "ERROR " << (index + 1) << '/' << gifs.size() << ": "
                  << object.key << " -> " << *failure << '\n';
      } else if (inspected && !inspected->normalizations.empty()) {
        ++normalized;
        const auto saved = preserve_finding(
            *source, findings_root, kNormalizedDirectory, object,
            source_is_temporary);
        auto record = base_record(object);
        record["status"] = "normalized";
        record["saved_original"] = saved.string();
        append_info(record, *inspected);
        write_record(report, record);
        std::cout << "NORMALIZED " << (index + 1) << '/' << gifs.size()
                  << ": " << object.key << " ("
                  << inspected->normalizations.size()
                  << " frame geometry adjustment(s)); fingerprint discarded\n";
      } else {
        const auto current = database.object(object.key);
        if (!current || current->remote.file_id != object.file_id ||
            current->remote.size != object.size ||
            current->remote.sha1 != object.sha1) {
          throw std::runtime_error(
              "The local inventory changed while a clean GIF fingerprint was "
              "being committed; no fingerprint was saved");
        }
        if (has_current_fingerprint(*current, config.fingerprint_version)) {
          ++clean_already_present;
        } else {
          database.save_fingerprint(object.key, object.file_id, *fingerprint);
          verify_saved_fingerprint(database, object, *fingerprint);
          ++clean_saved;
        }
        if (source_is_temporary && source && std::filesystem::exists(*source))
          remove_tool_file(*source);
      }

      if ((index + 1) % 100 == 0 || index + 1 == gifs.size()) {
        std::cout << "Progress: " << (index + 1) << '/' << gifs.size()
                  << " | clean_saved=" << clean_saved
                  << " normalized=" << normalized << " errors=" << failed
                  << " | preexisting=" << already_fingerprinted << '\n';
      }
    }

    write_record(report,
                 {{"type", "summary"},
                  {"total_gifs", gifs.size() + already_fingerprinted},
                  {"already_fingerprinted", already_fingerprinted},
                  {"swept", gifs.size()},
                  {"clean_saved", clean_saved},
                  {"clean_already_present", clean_already_present},
                  {"normalized", normalized},
                  {"errors", failed},
                  {"reused_saved_source", reused_saved_source},
                  {"findings_root", findings_root.string()}});

    std::error_code remove_error;
    std::filesystem::remove(temp_root, remove_error);

    std::cout << "\nDone. Report: " << report_path.string() << '\n'
              << "Already fingerprinted: " << already_fingerprinted
              << " | New clean fingerprints saved: " << clean_saved
              << " | Normalized (not saved to DB): " << normalized
              << " | Errors: " << failed << '\n'
              << "Special GIF originals: " << findings_root.string() << '\n';
    return failed == 0 ? 0 : 2;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe GIF sweep failed: " << problem.what() << '\n';
    return 1;
  }
}
