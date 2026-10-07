// installer/install_registry.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace eot::installer {

constexpr int kInstallSchemaVersion = 1;

constexpr const char *kInstallFolderName = "EdgeOfTimeRecompiled";

std::filesystem::path InstallRootFor(const std::filesystem::path &picked);

std::filesystem::path DefaultInstallRoot();

struct InstallConfig {
  std::filesystem::path install_root;
  std::string disc_fingerprint;
  int schema_version = 0;
  std::string app_version;

  std::filesystem::path game_data_path() const { return install_root / "game"; }
  std::filesystem::path profiles_path() const { return install_root / "profiles"; }
};

std::optional<InstallConfig> ReadInstallRegistry();

bool InstallIsPresent(const InstallConfig &config);

bool WriteInstallRegistry(const InstallConfig &config);

bool ClearInstallRegistry();

}

namespace eot::installer {

void RunUninstall();

}
