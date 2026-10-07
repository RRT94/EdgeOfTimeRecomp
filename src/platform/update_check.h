// platform/update_check.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace eot::platform {

struct InstallUpdate {
  std::string stamp;
  std::string version;
  std::string location;
};

void UpdateInstalledCopy(const std::filesystem::path &install_root);

std::optional<InstallUpdate> LastInstallUpdate();

struct BuildIdentity {
  std::string version;
  std::string commit;
  std::string stamp;
  bool modified = false;
};
std::optional<BuildIdentity> ReadBuildIdentity(const std::filesystem::path &exe);

}
