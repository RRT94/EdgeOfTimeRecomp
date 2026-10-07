// platform/desktop_shortcut.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/desktop_shortcut.h"

#if defined(_WIN32)
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

#include "core/encoding.h"

namespace eot::platform {
namespace {

std::wstring ShortcutFileName(std::string_view name) {
  std::wstring out = Utf8ToWide(name);
  std::erase_if(out, [](wchar_t c) {
    return c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' ||
           c == L'?' || c == L'*' || c < 32;
  });
  return out;
}

template <typename T> struct ComRelease {
  T *p;
  ~ComRelease() {
    if (p)
      p->Release();
  }
};

bool WriteShortcut(const std::filesystem::path &target, std::string_view name, std::string &error) {
  IShellLinkW *link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                              reinterpret_cast<void **>(&link)))) {
    error = "CoCreateInstance(CLSID_ShellLink) failed";
    return false;
  }
  ComRelease<IShellLinkW> link_guard{link};
  if (FAILED(link->SetPath(target.c_str()))) {
    error = "IShellLinkW::SetPath failed";
    return false;
  }
  if (FAILED(link->SetWorkingDirectory(target.parent_path().c_str()))) {
    error = "IShellLinkW::SetWorkingDirectory failed";
    return false;
  }
  IPersistFile *persist = nullptr;
  if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist)))) {
    error = "QueryInterface(IID_IPersistFile) failed";
    return false;
  }
  ComRelease<IPersistFile> persist_guard{persist};

  PWSTR desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
    error = "SHGetKnownFolderPath(FOLDERID_Desktop) failed";
    return false;
  }
  const std::wstring file = ShortcutFileName(name);
  const std::filesystem::path lnk = std::filesystem::path(desktop) / (file + L".lnk");
  CoTaskMemFree(desktop);
  if (file.empty()) {
    error = "the shortcut name is empty once the reserved characters are removed";
    return false;
  }
  if (FAILED(persist->Save(lnk.c_str(), TRUE))) {
    error = "IPersistFile::Save failed for " + lnk.string();
    return false;
  }
  return true;
}

}

bool CreateDesktopShortcut(const std::filesystem::path &target, std::string_view name,
                           std::string &error) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool ok = WriteShortcut(target, name, error);
  if (SUCCEEDED(init))
    CoUninitialize();
  return ok;
}

bool RemoveDesktopShortcut(std::string_view name) {
  const std::wstring file = ShortcutFileName(name);
  if (file.empty())
    return false;
  PWSTR desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
    return false;
  const std::filesystem::path lnk = std::filesystem::path(desktop) / (file + L".lnk");
  CoTaskMemFree(desktop);
  std::error_code ec;
  return std::filesystem::remove(lnk, ec);
}

}

#elif defined(__linux__)

#include <sys/stat.h>

#include <fstream>
#include <vector>

#include "platform/user_dirs.h"

namespace eot::platform {
namespace {

namespace fs = std::filesystem;

std::string ShortcutFileName(std::string_view name) {
  std::string out(name);
  std::erase_if(out, [](char c) { return c == '/' || c == '\\' || static_cast<unsigned char>(c) < 32; });
  return out;
}

std::vector<fs::path> EntryPaths(std::string_view name) {
  const std::string file = ShortcutFileName(name) + ".desktop";
  std::vector<fs::path> paths;
  if (const fs::path desktop = UserDir("XDG_DESKTOP_DIR", "Desktop"); !desktop.empty())
    paths.push_back(desktop / file);
  if (const fs::path data = DataHome(); !data.empty())
    paths.push_back(data / "applications" / file);
  return paths;
}

std::string EntryValue(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c == '\n' || c == '\r')
      continue;
    if (c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}

bool CreateDesktopShortcut(const fs::path &target, std::string_view name, std::string &error) {
  if (ShortcutFileName(name).empty()) {
    error = "the shortcut name is empty once the reserved characters are removed";
    return false;
  }
  const fs::path dir = target.parent_path();
  std::error_code ec;
  const fs::path icon = dir / "reeot_icon.png";
  std::string entry = "[Desktop Entry]\n"
                      "Type=Application\n"
                      "Version=1.0\n"
                      "Name=" + EntryValue(name) + "\n"
                      "Comment=Spider-Man: Edge of Time, recompiled for PC\n"
                      "Exec=\"" + EntryValue(target.string()) + "\"\n"
                      "Path=" + EntryValue(dir.string()) + "\n"
                      "Terminal=false\n"
                      "Categories=Game;\n";
  if (fs::is_regular_file(icon, ec))
    entry += "Icon=" + EntryValue(icon.string()) + "\n";
  const std::vector<fs::path> paths = EntryPaths(name);
  if (paths.empty()) {
    error = "no home folder to put the entry under";
    return false;
  }
  bool any = false;
  for (const fs::path &path : paths) {
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(entry.data(), static_cast<std::streamsize>(entry.size()))) {
      error = "could not write " + path.string();
      continue;
    }
    out.close();
    ::chmod(path.c_str(), 0755);
    any = true;
  }
  if (any)
    error.clear();
  return any;
}

bool RemoveDesktopShortcut(std::string_view name) {
  if (ShortcutFileName(name).empty())
    return false;
  bool any = false;
  std::error_code ec;
  for (const fs::path &path : EntryPaths(name))
    any |= fs::remove(path, ec);
  return any;
}

}

#else

namespace eot::platform {

bool CreateDesktopShortcut(const std::filesystem::path &, std::string_view, std::string &error) {
  error = "desktop shortcuts are only made on Windows and Linux";
  return false;
}

bool RemoveDesktopShortcut(std::string_view) { return false; }

}

#endif
