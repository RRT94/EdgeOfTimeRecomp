// installer/installer_music.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <filesystem>

namespace eot::installer {

class Music {
public:
  Music() = default;
  ~Music() { Stop(); }
  Music(const Music &) = delete;
  Music &operator=(const Music &) = delete;

  bool Start();

  void Update(float dt);

  void Stop();

  bool playing() const { return playing_; }

private:
  void SetVolume(int permille);

  std::filesystem::path file_;
  void *player_ = nullptr;
  bool playing_ = false;
  float level_ = 0.0f;
  int last_permille_ = -1;
};

}
