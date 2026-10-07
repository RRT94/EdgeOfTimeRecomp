// platform/process.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/process.h"

#include <SDL3/SDL.h>
#include <rex/filesystem.h>

#include "core/logging.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif

#include "platform/user_dirs.h"
#endif

namespace eot::platform {
namespace {

#if defined(_WIN32)
constexpr wchar_t kInstanceLockName[] = L"Local\\reeot-single-instance";
HANDLE g_instance_lock = nullptr;
#else
constexpr const char *kInstanceLockFile = "reeot-single-instance.lock";
int g_instance_lock = -1;
#endif

void ReleaseInstanceLock() {
#if defined(_WIN32)
  if (!g_instance_lock)
    return;
  ::CloseHandle(g_instance_lock);
  g_instance_lock = nullptr;
#else
  if (g_instance_lock < 0)
    return;
  ::close(g_instance_lock);
  g_instance_lock = -1;
#endif
}

bool SpawnProcess(const std::filesystem::path &exe) {
#if defined(_WIN32)
  std::wstring cmdline = L"\"" + exe.wstring() + L"\"";
  int argc = 0;
  if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      cmdline += L" \"";
      cmdline += argv[i];
      cmdline += L"\"";
    }
    LocalFree(argv);
  }
  std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
  buffer.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = exe.parent_path().wstring();
  if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    EOT_ERROR("[process] CreateProcessW failed for {} (error {})", exe.string(), GetLastError());
    return false;
  }
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  std::vector<std::string> args;
  args.push_back(exe.string());
#if defined(__APPLE__)
  {
    const int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();
    for (int i = 1; i < argc && argv && argv[i]; ++i)
      args.emplace_back(argv[i]);
  }
#else
  {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    bool first = true;
    while (pos < all.size()) {
      const size_t end = all.find('\0', pos);
      const std::string arg = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      if (!first)
        args.push_back(arg);
      first = false;
      if (end == std::string::npos)
        break;
      pos = end + 1;
    }
  }
#endif
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    EOT_ERROR("[process] fork failed for {} (errno {})", exe.string(), errno);
    return false;
  }
  if (pid == 0) {
    ::setsid();
    std::error_code ec;
    std::filesystem::current_path(exe.parent_path(), ec);
    ::execv(exe.c_str(), argv.data());
    ::_exit(127);
  }
  return true;
#endif
}

}

std::filesystem::path ProgramDir() { return rex::filesystem::GetExecutableFolder(); }

std::filesystem::path DataDir() {
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (InAppBundle())
    return exe_dir.parent_path() / "Resources";
  return exe_dir;
}

bool InAppBundle() {
#if defined(__APPLE__)
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (exe_dir.filename() != "MacOS")
    return false;
  const std::filesystem::path contents = exe_dir.parent_path();
  return contents.filename() == "Contents" && contents.parent_path().extension() == ".app";
#else
  return false;
#endif
}

bool AcquireInstanceLock() {
#if defined(_WIN32)
  HANDLE h = ::CreateMutexW(nullptr, FALSE, kInstanceLockName);
  if (!h) {
    EOT_WARN("[process] instance lock unavailable (error {})", ::GetLastError());
    return true;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(h);
    return false;
  }
  g_instance_lock = h;
  return true;
#else
  const std::string path = (RuntimeDir() / kInstanceLockFile).string();
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    EOT_WARN("[process] instance lock {} unavailable (errno {})", path, errno);
    return true;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  g_instance_lock = fd;
  return true;
#endif
}

bool SpawnReplacement(const std::filesystem::path &exe) {
  ReleaseInstanceLock();
  if (SpawnProcess(exe))
    return true;
  AcquireInstanceLock();
  return false;
}

std::filesystem::path LaunchPath() {
#if defined(__linux__)
  if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage)
    return std::filesystem::path(appimage);
#endif
  return rex::filesystem::GetExecutablePath();
}

bool RelaunchSelf() { return SpawnReplacement(LaunchPath()); }

void RaiseMainWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  if (windows && count > 0)
    SDL_RaiseWindow(windows[0]);
  SDL_free(windows);
}

}
