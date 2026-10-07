// gpu/patches/aspect_ratio.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

namespace eot::gpu {

float ConfiguredAspectRatio();

void ApplyAspectRatio();

bool LayoutIsWidescreen();

bool MacWide();

// How much wider than the game's own the FOV option draws the picture, and that
// camera's eye while it does.
float ViewWidening();

bool WidenedViewEye(float eye[3]);

class CameraRatioHold {
public:
  explicit CameraRatioHold(float value);
  ~CameraRatioHold();
  CameraRatioHold(const CameraRatioHold &) = delete;
  CameraRatioHold &operator=(const CameraRatioHold &) = delete;

private:
  unsigned saved_;
};

}

namespace eot::gpu {

bool TakeMovieDrawnFlag();

bool TakeMovieResolveSkip();

}
