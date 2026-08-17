#pragma once

#include "config.hpp"
#include "credentials.hpp"

#include <filesystem>

namespace gparty::fingerprints {

std::filesystem::path create_boot_install_payload(
    const Config &config, const B2Credentials &credentials);
void install_boot_worker(const std::filesystem::path &source_executable,
                         const std::filesystem::path &payload_path);
void report_boot_install_error(const std::filesystem::path &payload_path,
                               const std::string &message) noexcept;
void uninstall_boot_worker();
bool boot_worker_installed();
void run_elevated_boot_action(const std::filesystem::path &executable,
                              const std::wstring &action,
                              const std::filesystem::path &payload = {});

std::filesystem::path machine_config_path();
B2Credentials load_machine_credentials();

} // namespace gparty::fingerprints
