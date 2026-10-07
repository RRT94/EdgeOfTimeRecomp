// installer/install_registry.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "installer/install_registry.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#include <rex/filesystem.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "platform/desktop_shortcut.h"
#include "platform/fatal_dialog.h"
#include "platform/process.h"
#include "platform/user_dirs.h"

namespace eot::installer {

std::filesystem::path InstallRootFor(const std::filesystem::path &picked) {
  if (picked.filename() == kInstallFolderName)
    return picked;
  return picked / kInstallFolderName;
}

std::filesystem::path DefaultInstallRoot() {
#if defined(__APPLE__)
  if (const std::filesystem::path home = platform::HomeDir(); !home.empty()) {
    std::error_code ec;
    const std::filesystem::path support = home / "Library" / "Application Support";
    return std::filesystem::is_directory(support, ec) ? support / kInstallFolderName
                                                      : home / ("." + std::string(kInstallFolderName));
  }
#elif defined(__linux__)
  if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage)
    if (const std::filesystem::path data = platform::DataHome(); !data.empty())
      return data / kInstallFolderName;
#endif
  return rex::filesystem::GetExecutableFolder();
}

}

#if defined(_WIN32)
#include <windows.h>

#include "core/encoding.h"

namespace eot::installer {
namespace {

constexpr wchar_t kInstallKey[] = L"Software\\reeot\\Install";

std::optional<std::wstring> ReadString(HKEY key, const wchar_t *name) {
  DWORD type = 0, size = 0;
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS)
    return std::nullopt;
  std::wstring out(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, out.data(), &size) != ERROR_SUCCESS)
    return std::nullopt;
  while (!out.empty() && out.back() == L'\0')
    out.pop_back();
  return out;
}

bool WriteString(HKEY key, const wchar_t *name, const std::wstring &value) {
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), bytes) ==
         ERROR_SUCCESS;
}

struct KeyGuard {
  HKEY key;
  ~KeyGuard() {
    if (key)
      RegCloseKey(key);
  }
};

}

std::optional<InstallConfig> ReadInstallRegistry() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kInstallKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    return std::nullopt;
  KeyGuard guard{key};

  const auto root = ReadString(key, L"InstallRoot");
  if (!root || root->empty())
    return std::nullopt;

  InstallConfig cfg;
  cfg.install_root = *root;
  if (auto fp = ReadString(key, L"DiscFingerprint"))
    cfg.disc_fingerprint = WideToUtf8(*fp);
  if (auto sv = ReadString(key, L"SchemaVersion")) {
    try {
      cfg.schema_version = std::stoi(*sv);
    } catch (...) {
      cfg.schema_version = 0;
    }
  }
  if (auto v = ReadString(key, L"AppVersion"))
    cfg.app_version = WideToUtf8(*v);

  return cfg;
}

bool InstallIsPresent(const InstallConfig &config) {
  std::error_code ec;
  return std::filesystem::is_regular_file(config.game_data_path() / "Default.xex", ec);
}

bool WriteInstallRegistry(const InstallConfig &config) {
  HKEY key = nullptr;
  const LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kInstallKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    EOT_ERROR("[install] RegCreateKeyExW failed: {}", status);
    return false;
  }
  {
    KeyGuard guard{key};
    bool ok = true;
    ok &= WriteString(key, L"InstallRoot", config.install_root.wstring());
    ok &= WriteString(key, L"DiscFingerprint", Utf8ToWide(config.disc_fingerprint));
    ok &= WriteString(key, L"SchemaVersion", std::to_wstring(kInstallSchemaVersion));
    ok &= WriteString(key, L"AppVersion", Utf8ToWide(REEOT_VERSION_STRING));
    if (ok)
      return true;
    EOT_ERROR("[install] could not write every value of the install record");
  }
  ClearInstallRegistry();
  return false;
}

bool ClearInstallRegistry() {
  const LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, kInstallKey);
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
    return true;
  EOT_ERROR("[install] RegDeleteTreeW failed: {}", status);
  return false;
}

}

#else

#include <fstream>
#include <string_view>
#include <system_error>


namespace eot::installer {
namespace {

namespace fs = std::filesystem;

fs::path InstallRecordPath() {
  const fs::path config = platform::ConfigHome();
  return config.empty() ? fs::path() : config / "reeot" / "install.toml";
}

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

bool ParseLine(std::string_view line, std::string &key, std::string &value) {
  const auto trim = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
      v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
      v.remove_suffix(1);
    return v;
  };
  line = trim(line);
  if (line.empty() || line.front() == '#')
    return false;
  const size_t eq = line.find('=');
  if (eq == std::string_view::npos)
    return false;
  key = std::string(trim(line.substr(0, eq)));
  std::string_view raw = trim(line.substr(eq + 1));
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    raw = raw.substr(1, raw.size() - 2);
    value.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size())
        ++i;
      value.push_back(raw[i]);
    }
  } else {
    value = std::string(raw);
  }
  return !key.empty();
}

}

std::optional<InstallConfig> ReadInstallRegistry() {
  const fs::path path = InstallRecordPath();
  if (path.empty())
    return std::nullopt;
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  InstallConfig cfg;
  std::string line, key, value;
  while (std::getline(in, line)) {
    if (!ParseLine(line, key, value))
      continue;
    if (key == "install_root")
      cfg.install_root = value;
    else if (key == "disc_fingerprint")
      cfg.disc_fingerprint = value;
    else if (key == "app_version")
      cfg.app_version = value;
    else if (key == "schema_version")
      cfg.schema_version = std::atoi(value.c_str());
  }
  if (cfg.install_root.empty())
    return std::nullopt;
  return cfg;
}

bool InstallIsPresent(const InstallConfig &config) {
  std::error_code ec;
  return fs::is_regular_file(config.game_data_path() / "Default.xex", ec);
}

bool WriteInstallRegistry(const InstallConfig &config) {
  const fs::path path = InstallRecordPath();
  if (path.empty()) {
    EOT_ERROR("[install] no HOME or XDG_CONFIG_HOME to keep the install record in");
    return false;
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "# Where reeot is installed for this user; written by the installer.\n"
        << "install_root = " << Quote(config.install_root.string()) << "\n"
        << "disc_fingerprint = " << Quote(config.disc_fingerprint) << "\n"
        << "schema_version = " << kInstallSchemaVersion << "\n"
        << "app_version = " << Quote(REEOT_VERSION_STRING) << "\n";
    if (!out) {
      EOT_ERROR("[install] could not write {}", tmp.string());
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    EOT_ERROR("[install] could not move the install record into place: {}", ec.message());
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool ClearInstallRegistry() {
  const fs::path path = InstallRecordPath();
  if (path.empty())
    return true;
  std::error_code ec;
  fs::remove(path, ec);
  return !ec || ec == std::errc::no_such_file_or_directory;
}

}

#endif

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
fs::path DownloadsFolder() {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)))
    out = p;
  if (p)
    CoTaskMemFree(p);
  if (out.empty())
    if (const wchar_t *home = _wgetenv(L"USERPROFILE"); home && *home)
      out = fs::path(home) / L"Downloads";
  return out;
}
#else
fs::path DownloadsFolder() { return platform::UserDir("XDG_DOWNLOAD_DIR", "Downloads"); }
#endif

fs::path UniqueDir(const fs::path &parent, const std::string &base) {
  std::error_code ec;
  fs::path p = parent / base;
  for (int n = 2; fs::exists(p, ec); ++n)
    p = parent / (base + " (" + std::to_string(n) + ")");
  return p;
}

size_t CopySaves(const fs::path &profiles, const fs::path &dest) {
  std::error_code ec;
  size_t files = 0;
  for (fs::recursive_directory_iterator it(profiles, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec)
      break;
    if (it->is_directory(ec)) {
      if (it->path().filename() == "cache")
        it.disable_recursion_pending();
      continue;
    }
    const fs::path rel = fs::relative(it->path(), profiles, ec);
    if (ec)
      continue;
    const fs::path out = dest / rel;
    fs::create_directories(out.parent_path(), ec);
    if (fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec))
      ++files;
  }
  return files;
}

void SpawnDeferredDelete(const fs::path &root) {
#if defined(_WIN32)
  const std::wstring q = L"\"" + root.wstring() + L"\"";
  std::wstring cmd = L"cmd.exe /c \"for /l %i in (1,1,30) do (rmdir /s /q " + q + L" 2>nul & if not exist " +
                     q + L" exit & ping 127.0.0.1 -n 2 >nul)\"";
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
#else
  const pid_t pid = ::fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    ::setsid();
    const std::string script = "for i in $(seq 1 30); do rm -rf \"$0\" 2>/dev/null; [ -e \"$0\" ] || exit 0; sleep 1; done";
    ::execl("/bin/sh", "sh", "-c", script.c_str(), root.c_str(), static_cast<char *>(nullptr));
    ::_exit(127);
  }
#endif
}

}

void RunUninstall() {
  std::error_code ec;
  fs::path root;
  if (auto cfg = ReadInstallRegistry())
    root = cfg->install_root;
  if (root.empty()) {
    const fs::path prog = platform::ProgramDir();
    if (prog.filename() == kInstallFolderName)
      root = prog;
  }
  if (root.empty() || !fs::exists(root, ec)) {
    ClearInstallRegistry();
    platform::ShowInfo("reeot", "No reeot installation was found to remove.");
    return;
  }

  if (!platform::ShowConfirm(
          "Uninstall reeot",
          "This removes the installation at:\n\n" + root.string() +
              "\n\nYour saves are copied to your Downloads folder first. Continue?"))
    return;

  std::string saved_to;
  const fs::path profiles = root / "profiles";
  if (fs::is_directory(profiles, ec)) {
    if (const fs::path downloads = DownloadsFolder(); !downloads.empty()) {
      const fs::path dest = UniqueDir(downloads, "reeot saves");
      if (CopySaves(profiles, dest) > 0)
        saved_to = dest.string();
      else
        fs::remove_all(dest, ec);
    }
  }

  ClearInstallRegistry();
  platform::RemoveDesktopShortcut("reeot");

  fs::remove_all(root, ec);
  const bool deferred = fs::exists(root, ec);
  if (deferred)
    SpawnDeferredDelete(root);

  std::string msg = "reeot has been uninstalled.";
  msg += saved_to.empty() ? "\n\nNo saves were found to keep."
                          : "\n\nYour saves were copied to:\n" + saved_to;
  if (deferred)
    msg += "\n\nThe last program files are removed a moment after this window closes.";
  EOT_INFO("[uninstall] removed {} (saves: {}{})", root.string(),
           saved_to.empty() ? "none" : saved_to, deferred ? "; deferred" : "");
  platform::ShowInfo("reeot - uninstalled", msg);
}

}
