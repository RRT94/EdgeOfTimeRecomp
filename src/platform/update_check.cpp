// platform/update_check.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/update_check.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "platform/process.h"

REXCVAR_DEFINE_BOOL(eot_update_apply, true, "EdgeOfTime/Config", "Copy newer builds over install");

namespace eot::platform {

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::optional<InstallUpdate> g_updated;

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n ? n - 1 : 0, '\0');
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::vector<uint8_t> ReadVersionResource(const fs::path &exe) {
  DWORD unused = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &unused);
  if (!size)
    return {};
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
    return {};
  return data;
}

std::string ResourceString(const std::vector<uint8_t> &res, const wchar_t *key) {
  if (res.empty())
    return {};
  void *value = nullptr;
  UINT len = 0;
  const std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
  if (!VerQueryValueW(res.data(), path.c_str(), &value, &len) || !value || len == 0)
    return {};
  return WideToUtf8(std::wstring(static_cast<const wchar_t *>(value), len - 1));
}

std::string InstalledBuildStamp(const fs::path &exe) {
  return ResourceString(ReadVersionResource(exe), L"BuildStamp");
}
#else
std::string InstalledBuildStamp(const fs::path &exe) {
  std::ifstream in(exe.parent_path() / "build_stamp.txt");
  std::string stamp;
  std::getline(in, stamp);
  while (!stamp.empty() && (stamp.back() == '\r' || stamp.back() == ' ' || stamp.back() == '\t'))
    stamp.pop_back();
  return stamp;
}
#endif

std::vector<std::string> ProgramFiles(const fs::path &dir) {
  std::vector<std::string> files;
  std::ifstream in(dir / "program_files.txt");
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (!line.empty())
      files.push_back(line);
  }
  if (files.empty()) {
#if defined(_WIN32)
    files = {"EdgeOfTimeRecomp.exe", "rexruntimerd.dll", "reeot_GameLogic.dll",
             "dxcompiler.dll",       "dxil.dll",         "gamecontrollerdb.txt"};
#elif defined(__APPLE__)
    files = {"EdgeOfTimeRecomp", "librexruntime.dylib", "reeot_GameLogic", "gamecontrollerdb.txt",
             "build_stamp.txt", "reeot_icon.png", "vulkan/lib/libvulkan.1.dylib", "vulkan/lib/libMoltenVK.dylib",
             "vulkan/share/vulkan/icd.d/MoltenVK_icd.json"};
#else
    files = {"EdgeOfTimeRecomp", "librexruntime.so", "reeot_GameLogic", "gamecontrollerdb.txt",
             "build_stamp.txt", "reeot_icon.png"};
#endif
  }
  return files;
}

bool FilesDiffer(const fs::path &have, const fs::path &src) {
  std::error_code ec;
  if (!fs::exists(have, ec))
    return true;
  if (fs::file_size(have, ec) != fs::file_size(src, ec) || ec)
    return true;
  std::ifstream a(have, std::ios::binary), b(src, std::ios::binary);
  if (!a || !b)
    return true;
  char ba[64 * 1024], bb[64 * 1024];
  for (;;) {
    a.read(ba, sizeof(ba));
    b.read(bb, sizeof(bb));
    if (a.gcount() != b.gcount() || std::memcmp(ba, bb, static_cast<size_t>(a.gcount())) != 0)
      return true;
    if (a.eof() && b.eof())
      return false;
    if (a.bad() || b.bad())
      return true;
  }
}

void ClearUpdateLeftovers(const fs::path &dir, const std::vector<std::string> &files) {
  std::error_code ec;
  for (const auto &f : files)
    fs::remove(dir / (f + ".old"), ec);
}

bool ReplaceFile(const fs::path &src, const fs::path &dst, const fs::path &aside) {
  std::error_code ec;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (!ec)
    return true;
  ec.clear();
  fs::remove(aside, ec);
  fs::rename(dst, aside, ec);
  if (ec)
    return false;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

}

void UpdateInstalledCopy(const fs::path &install_root) {
#if !defined(_WIN32)
  (void)install_root;
  return;
#else
  if (!REXCVAR_GET(eot_update_apply) || install_root.empty())
    return;
  std::error_code ec;
  const fs::path prog = ProgramDir();
  const std::vector<std::string> files = ProgramFiles(prog);

  ClearUpdateLeftovers(install_root, files);

  if (fs::equivalent(prog, install_root, ec))
    return;
  const fs::path installed_exe = install_root / kExecutableFileName;
  if (!fs::is_regular_file(installed_exe, ec))
    return;

  const std::string installed = InstalledBuildStamp(installed_exe);
  if (!installed.empty() && installed >= std::string(REEOT_BUILD_TIMESTAMP))
    return;

  size_t pushed = 0;
  for (const auto &f : files) {
    const fs::path src = prog / f;
    if (!fs::is_regular_file(src, ec) || !FilesDiffer(install_root / f, src))
      continue;
    if (ReplaceFile(src, install_root / f, install_root / (f + ".old")))
      ++pushed;
    else
      EOT_ERROR("[update] could not replace {} in {}", f, install_root.string());
  }
  if (pushed == 0)
    return;

  {
    std::lock_guard lock(g_mutex);
    g_updated = InstallUpdate{REEOT_BUILD_TIMESTAMP, REEOT_VERSION_STRING, install_root.string()};
  }
  EOT_INFO("[update] updated the install at {} to v{} build {} ({} file(s))", install_root.string(),
           REEOT_VERSION_STRING, REEOT_BUILD_TIMESTAMP, pushed);
#endif
}

std::optional<BuildIdentity> ReadBuildIdentity(const fs::path &exe) {
  BuildIdentity id;
#if defined(_WIN32)
  const std::vector<uint8_t> res = ReadVersionResource(exe);
  id.stamp = ResourceString(res, L"BuildStamp");
  id.version = ResourceString(res, L"FileVersion");
  id.commit = ResourceString(res, L"Commit");
  id.modified = ResourceString(res, L"Modified") == "1";
#else
  id.stamp = InstalledBuildStamp(exe);
#endif
  if (id.stamp.empty() && id.version.empty())
    return std::nullopt;
  return id;
}

std::optional<InstallUpdate> LastInstallUpdate() {
  std::lock_guard lock(g_mutex);
  return g_updated;
}

}
