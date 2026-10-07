// installer/self_install.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "installer/self_install.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>

#include "core/logging.h"
#include "embedded.h"
#include "embedded_menu_package.h"
#include "embedded_achievements_package.h"
#include "embedded_icons_package.h"
#include "embedded_suits_package.h"
#include "embedded_package.h"
#include "goliath/ui/menu_handles.h"
#include "installer/program_files.h"
#include "platform/process.h"

namespace eot::installer {

namespace fs = std::filesystem;

std::vector<std::string> MissingProgramFiles() {
  const fs::path here = rex::filesystem::GetExecutableFolder();
  const fs::path data = platform::DataDir();
  std::vector<std::string> missing;
  std::error_code ec;
  for (const char *rel : kProgramFiles)
    if (!fs::exists(here / rel, ec) && !fs::exists(data / rel, ec))
      missing.push_back(rel);
  return missing;
}

bool CopyProgramTo(const fs::path &install, std::string &error) {
  const fs::path here = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  if (!here.empty() && fs::equivalent(here, install, ec))
    return true;

  size_t copied = 0;
  for (const char *rel : kProgramFiles) {
    const fs::path src = here / rel;
    const fs::path dst = install / rel;
    if (!fs::exists(src, ec)) {
      error = std::string(rel) + " is not beside " + here.string();
      return false;
    }
    fs::create_directories(dst.parent_path(), ec);
    if (fs::is_directory(src, ec)) {
      fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      error = src.string() + " -> " + dst.string() + " (" + ec.message() + ")";
      return false;
    }
    ++copied;
  }
  EOT_INFO("[install] copied {} program files into {}", copied, install.string());
  return true;
}

namespace {

bool FileHolds(const fs::path &path, std::span<const uint8_t> bytes) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != bytes.size())
    return false;
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> have(bytes.size());
  in.read(reinterpret_cast<char *>(have.data()), static_cast<std::streamsize>(have.size()));
  return in.good() && std::equal(have.begin(), have.end(), bytes.begin());
}

void WriteAsset(const EmbeddedAsset &asset, const fs::path &path, size_t &written) {
  if (FileHolds(path, asset.bytes()))
    return;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(asset.data), static_cast<std::streamsize>(asset.size));
  if (!out) {
    EOT_WARN("[install] could not write {}", path.string());
    return;
  }
  ++written;
}

}

void WritePortFiles(const fs::path &game) {
  size_t written = 0;
  const EmbeddedAsset packages[] = {EmbeddedPortPackage(), EmbeddedMenuPackage(), EmbeddedAchievementsPackage(),
                                    EmbeddedIconsPackage(), EmbeddedSuitsPackage()};
  for (const EmbeddedAsset &package : packages) {
    const fs::path name = fs::path(std::string(package.name)).filename();
    WriteAsset(package, game / "Data" / eot::ui::kReeotPackageFolder / name, written);
    std::error_code stale_ec;
    const fs::path stale = game / "Data" / name;
    if (fs::is_regular_file(stale, stale_ec) && fs::remove(stale, stale_ec))
      EOT_INFO("[install] removed the old {}", stale.string());
  }
  if (written)
    EOT_INFO("[install] {} port file(s) written for {}", written, game.string());
}

void AdoptLegacyUserData(const fs::path &profile) {
  const fs::path legacy = rex::filesystem::GetUserFolder() / "reeot";
  std::error_code ec;
  if (!fs::is_directory(legacy, ec) || fs::equivalent(legacy, profile, ec))
    return;
  size_t adopted = 0;
  for (const auto &it : fs::directory_iterator(legacy, ec)) {
    const std::string name = it.path().filename().string();
    if (!it.is_directory() || name == "cache" || name.find('.') != std::string::npos)
      continue;
    const fs::path dst = profile / name;
    if (fs::exists(dst, ec))
      continue;
    fs::copy(it.path(), dst, fs::copy_options::recursive, ec);
    if (ec) {
      EOT_WARN("[install] could not adopt {} from {}: {}", name, legacy.string(), ec.message());
      ec.clear();
      continue;
    }
    ++adopted;
  }
  if (adopted)
    EOT_INFO("[install] adopted {} folder(s) of saves from {} into {}", adopted, legacy.string(), profile.string());
}

}
