// installer/installer_music.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "installer/installer_music.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#include "core/logging.h"
#include "embedded.h"
#if defined(__APPLE__)
#include "installer/installer_music_mac.h"
#endif

namespace eot::installer {

namespace {

constexpr float kFadeInSeconds = 4.0f;
constexpr int kFullPermille = 750;

constexpr const wchar_t *kAlias = L"reeot_installer_music";

#if defined(_WIN32)
bool Mci(const std::wstring &command) {
  const MCIERROR error = ::mciSendStringW(command.c_str(), nullptr, 0, nullptr);
  if (error) {
    wchar_t text[256] = {};
    ::mciGetErrorStringW(error, text, 256);
    char narrow[256] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow), nullptr, nullptr);
    EOT_WARN("[install] music: mci refused '{}': {}", std::string(command.begin(), command.end()), narrow);
    return false;
  }
  return true;
}
#endif

}

bool Music::Start() {
#if defined(_WIN32)
  if (playing_)
    return true;
  constexpr auto kClip = eot::Embedded("installer/installer.mp3");
  if (!kClip.size)
    return false;
  std::error_code ec;
  file_ = std::filesystem::temp_directory_path(ec) / "reeot_installer.mp3";
  if (ec)
    return false;
  {
    std::ofstream out(file_, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char *>(kClip.data), static_cast<std::streamsize>(kClip.size)))
      return false;
  }
  if (!Mci(L"open \"" + file_.wstring() + L"\" type mpegvideo alias " + kAlias))
    return false;
  playing_ = true;
  level_ = 0.0f;
  last_permille_ = -1;
  SetVolume(0);
  if (!Mci(std::wstring(L"play ") + kAlias)) {
    Stop();
    return false;
  }
  EOT_INFO("[install] music playing from {}", file_.string());
  return true;
#elif defined(__APPLE__)
  if (playing_)
    return true;
  constexpr auto kClip = eot::Embedded("installer/installer.mp3");
  std::string error;
  player_ = mac::StartPlayer(kClip.data, kClip.size, error);
  if (!player_) {
    EOT_WARN("[install] music: AVFoundation would not play the clip: {}", error);
    return false;
  }
  playing_ = true;
  level_ = 0.0f;
  last_permille_ = 0;
  EOT_INFO("[install] music playing");
  return true;
#else
  return false;
#endif
}

void Music::Update(float dt) {
  if (!playing_)
    return;
  level_ = std::min(1.0f, level_ + dt / kFadeInSeconds);
  SetVolume(static_cast<int>(level_ * level_ * kFullPermille));
}

void Music::SetVolume(int permille) {
#if defined(_WIN32)
  if (!playing_ || permille == last_permille_)
    return;
  last_permille_ = permille;
  Mci(std::wstring(L"setaudio ") + kAlias + L" volume to " + std::to_wstring(permille));
#elif defined(__APPLE__)
  if (!playing_ || permille == last_permille_)
    return;
  last_permille_ = permille;
  mac::SetPlayerVolume(player_, static_cast<float>(permille) / 1000.0f);
#else
  (void)permille;
#endif
}

void Music::Stop() {
#if defined(_WIN32)
  if (!playing_)
    return;
  playing_ = false;
  Mci(std::wstring(L"close ") + kAlias);
  std::error_code ec;
  std::filesystem::remove(file_, ec);
  EOT_INFO("[install] music stopped");
#elif defined(__APPLE__)
  if (!playing_)
    return;
  playing_ = false;
  mac::StopPlayer(player_);
  player_ = nullptr;
  EOT_INFO("[install] music stopped");
#endif
}

}
