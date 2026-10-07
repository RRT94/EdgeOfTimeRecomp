// installer/dlc_publish.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "installer/dlc_publish.h"

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/string/utf8.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "installer/disc_install.h"

namespace eot::installer {
namespace {

namespace fs = std::filesystem;
using namespace rex;
using namespace rex::system;
using namespace rex::filesystem;

constexpr const char *kMarketplaceXuid = "0000000000000000";
constexpr const char *kHeadersDir = "Headers";
constexpr size_t kContentNameLength = 42;

void CollectFiles(Entry *dir, const std::string &prefix, std::vector<std::pair<std::string, Entry *>> &out) {
  for (const auto &child : dir->children()) {
    const std::string child_path = prefix.empty() ? child->name() : prefix + "/" + child->name();
    if (child->attributes() & kFileAttributeDirectory)
      CollectFiles(child.get(), child_path, out);
    else
      out.emplace_back(child_path, child.get());
  }
}

bool WriteEntry(Entry *entry, const fs::path &dest) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  File *file = nullptr;
  if (entry->Open(FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS || !file)
    return false;
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) {
    file->Destroy();
    return false;
  }
  std::vector<uint8_t> buffer(1u << 20);
  size_t offset = 0;
  const size_t size = entry->size();
  bool ok = true;
  while (offset < size) {
    size_t read = 0;
    const size_t want = std::min(buffer.size(), size - offset);
    if (file->ReadSync(std::span<uint8_t>(buffer.data(), want), offset, &read) != X_STATUS_SUCCESS || read == 0) {
      ok = false;
      break;
    }
    out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(read));
    if (!out) {
      ok = false;
      break;
    }
    offset += read;
  }
  file->Destroy();
  return ok;
}

bool WriteHeader(const fs::path &path, const StfsHeader &stfs, const std::string &name) {
  xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = static_cast<uint32_t>(xam::DummyDeviceId::HDD);
  data.content_type = XContentType::kMarketplaceContent;
  data.title_id = stfs.metadata.execution_info.title_id;
  data.xuid = 0;
  data.set_display_name(stfs.metadata.display_name(XLanguage::kEnglish));
  data.set_file_name(name);
  uint32_t license_mask = 0;
  for (const auto &license : stfs.header.licenses)
    if (license.license_flags)
      license_mask |= license.license_bits;

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(reinterpret_cast<const char *>(&data), sizeof(data));
  out.write(reinterpret_cast<const char *>(&license_mask), sizeof(license_mask));
  return out.good();
}

void AdoptFromGameFolder(const fs::path &dlc_dir, const fs::path &game) {
  std::error_code ec;
  const fs::path old = game / "Content" / kMarketplaceXuid / std::format("{:08X}", kTitleId) /
                       std::format("{:08X}", kContentTypeMarketplace);
  if (!fs::is_directory(old, ec))
    return;
  for (const auto &it : fs::directory_iterator(old, ec)) {
    if (!it.is_regular_file())
      continue;
    fs::create_directories(dlc_dir, ec);
    const fs::path dest = dlc_dir / it.path().filename();
    fs::rename(it.path(), dest, ec);
    if (ec) {
      EOT_WARN("[dlc] could not move {} out of the game folder: {}", it.path().string(), ec.message());
      ec.clear();
      continue;
    }
    EOT_INFO("[dlc] {} moved out of the game folder into {}", it.path().filename().string(), dlc_dir.string());
  }
}

}

void PublishDlc(const fs::path &dlc_dir, const fs::path &game, const fs::path &profile) {
  AdoptFromGameFolder(dlc_dir, game);

  std::error_code ec;
  if (profile.empty() || !fs::is_directory(dlc_dir, ec))
    return;
  const fs::path title = profile / kMarketplaceXuid / std::format("{:08X}", kTitleId);
  const std::string type = std::format("{:08X}", kContentTypeMarketplace);
  const fs::path content_dir = title / type;
  const fs::path headers_dir = title / kHeadersDir / type;

  for (const auto &it : fs::directory_iterator(dlc_dir, ec)) {
    if (!it.is_regular_file())
      continue;
    const fs::path &package = it.path();
    auto header = StfsContainerDevice::ReadPackageHeader(package);
    if (!header || header->metadata.execution_info.title_id != kTitleId ||
        static_cast<uint32_t>(static_cast<XContentType>(header->metadata.content_type)) !=
            kContentTypeMarketplace) {
      EOT_WARN("[dlc] {} is not this game's downloadable content; left alone", package.string());
      continue;
    }
    std::string name = package.filename().string();
    if (name.size() > kContentNameLength)
      name.resize(kContentNameLength);
    const fs::path folder = content_dir / name;
    const fs::path header_path = headers_dir / (name + ".header");
    if (fs::is_directory(folder, ec) && fs::is_regular_file(header_path, ec))
      continue;

    const std::string display_name = rex::string::to_utf8(header->metadata.display_name(XLanguage::kEnglish));
    StfsContainerDevice device("", package);
    Entry *root = device.Initialize() ? device.ResolvePath("") : nullptr;
    if (!root) {
      EOT_WARN("[dlc] {} could not be opened as a content package", package.string());
      continue;
    }
    std::vector<std::pair<std::string, Entry *>> files;
    CollectFiles(root, "", files);

    fs::remove_all(folder, ec);
    bool ok = true;
    std::string listed;
    for (const auto &[rel, entry] : files) {
      if (!WriteEntry(entry, folder / fs::path(rel))) {
        EOT_WARN("[dlc] {}: could not extract {}", package.filename().string(), rel);
        ok = false;
        break;
      }
      listed += (listed.empty() ? "" : ", ") + rel;
    }
    if (ok)
      ok = WriteHeader(header_path, *header, name);
    if (!ok) {
      fs::remove_all(folder, ec);
      fs::remove(header_path, ec);
      EOT_WARN("[dlc] {} was not published", package.filename().string());
      continue;
    }
    EOT_INFO("[dlc] published '{}' ({} files: {}) into {}", display_name, files.size(), listed,
             content_dir.string());
  }
}

}
