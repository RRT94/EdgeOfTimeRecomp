// goliath/debug/freecam.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/mouse_input.h"
#include "goliath/debug/freecam.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL.h>
#include <atomic>
#include <bit>
#include <string>
#include <charconv>
#include <cstdlib>
#include <format>
#include <vector>
#include "goliath/controller/pad_actions.h"
#endif

REX_EXTERN(__imp__eot_GRMainCamera_ViewBegin);
REX_EXTERN(__imp__eot_Renderer_SetupCamera);
REX_EXTERN(__imp__eot_Renderer_Present);

REXCVAR_DEFINE_BOOL(eot_freecam, false, "EdgeOfTime/Debug", "Fly the camera around");

REXCVAR_DEFINE_DOUBLE(eot_freecam_speed, 8.0, "EdgeOfTime/Debug", "Free camera move speed");

REXCVAR_DEFINE_BOOL(eot_freecam_two_sided, true, "EdgeOfTime/Debug", "Draw backfaces while flying");

REXCVAR_DEFINE_DOUBLE(eot_freecam_turn_speed, 120.0, "EdgeOfTime/Debug", "Free camera turn speed");

REXCVAR_DEFINE_DOUBLE(eot_freecam_mouse_speed, 0.12, "EdgeOfTime/Debug", "Free camera mouse speed");

namespace {

constexpr uint32_t kCommandCameraOwner = 0x2E8;
constexpr uint32_t kActiveCameraRecord = 2124;
constexpr uint32_t kWorldMatrix = 24;

constexpr uint32_t kCameraList = 0x8249C1AC;
constexpr int kCameraListEntries = 16;
constexpr uint32_t kCameraFlags = 412;
constexpr uint32_t kFlagPublished = 1u << 28;
constexpr uint32_t kFlagSkip = 1u << 2;

constexpr double kPi = 3.14159265358979323846;
constexpr double kPitchLimit = kPi * 0.5 - 0.01;

struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 g_pos;
double g_yaw = 0.0;
double g_pitch = 0.0;

bool g_active = false;
double g_speed_shown = 0.0;

float g_frame_rows[16] = {};
bool g_frame_rows_valid = false;
bool g_need_seed = false;

constexpr int kMaxCameras = 8;

constexpr uint64_t kFreshFrames = 1;

struct Adopted {
  uint32_t camera = 0;
  uint64_t last_seen = 0;
  float saved[16] = {};
  float written[16] = {};
  bool ever_written = false;
};

Adopted g_cams[kMaxCameras];
int g_cam_count = 0;
uint64_t g_frame = 0;

std::mutex g_cams_mutex;

bool Fresh(const Adopted &a) { return g_frame - a.last_seen <= kFreshFrames; }

Adopted *FindCamera(uint32_t camera) {
  for (int i = 0; i < g_cam_count; ++i)
    if (g_cams[i].camera == camera)
      return &g_cams[i];
  return nullptr;
}

void ExpireCameras() {
  int keep = 0;
  for (int i = 0; i < g_cam_count; ++i)
    if (Fresh(g_cams[i]))
      g_cams[keep++] = g_cams[i];
  g_cam_count = keep;
}

enum class Key { Shift, Control, Left, Right, Up, Down, W, A, S, D, Q, E, R };

#if defined(_WIN32)
int NativeKey(Key key) {
  switch (key) {
  case Key::Shift:
    return VK_SHIFT;
  case Key::Control:
    return VK_CONTROL;
  case Key::Left:
    return VK_LEFT;
  case Key::Right:
    return VK_RIGHT;
  case Key::Up:
    return VK_UP;
  case Key::Down:
    return VK_DOWN;
  case Key::W:
    return 'W';
  case Key::A:
    return 'A';
  case Key::S:
    return 'S';
  case Key::D:
    return 'D';
  case Key::Q:
    return 'Q';
  case Key::E:
    return 'E';
  case Key::R:
    return 'R';
  }
  return 0;
}
#else
SDL_Scancode NativeKey(Key key) {
  switch (key) {
  case Key::Shift:
    return SDL_SCANCODE_LSHIFT;
  case Key::Control:
    return SDL_SCANCODE_LCTRL;
  case Key::Left:
    return SDL_SCANCODE_LEFT;
  case Key::Right:
    return SDL_SCANCODE_RIGHT;
  case Key::Up:
    return SDL_SCANCODE_UP;
  case Key::Down:
    return SDL_SCANCODE_DOWN;
  case Key::W:
    return SDL_SCANCODE_W;
  case Key::A:
    return SDL_SCANCODE_A;
  case Key::S:
    return SDL_SCANCODE_S;
  case Key::D:
    return SDL_SCANCODE_D;
  case Key::Q:
    return SDL_SCANCODE_Q;
  case Key::E:
    return SDL_SCANCODE_E;
  case Key::R:
    return SDL_SCANCODE_R;
  }
  return SDL_SCANCODE_UNKNOWN;
}
#endif

bool KeyDown(Key key) {
#if defined(_WIN32)
  return (GetAsyncKeyState(NativeKey(key)) & 0x8000) != 0;
#else
  const bool *state = SDL_GetKeyboardState(nullptr);
  const SDL_Scancode code = NativeKey(key);
  if (!state || code == SDL_SCANCODE_UNKNOWN)
    return false;
  if (state[code])
    return true;
  if (code == SDL_SCANCODE_LSHIFT)
    return state[SDL_SCANCODE_RSHIFT];
  if (code == SDL_SCANCODE_LCTRL)
    return state[SDL_SCANCODE_RCTRL];
  return false;
#endif
}

bool WindowHasFocus() {
#if defined(_WIN32)
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
#else
  SDL_Window *focused = SDL_GetKeyboardFocus();
  return focused != nullptr;
#endif
}

double Axis(Key positive, Key negative) {
  return (KeyDown(positive) ? 1.0 : 0.0) - (KeyDown(negative) ? 1.0 : 0.0);
}

double HostDelta() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const clock::time_point now = clock::now();
  if (last.time_since_epoch().count() == 0) {
    last = now;
    return 0.0;
  }
  const double dt = std::chrono::duration<double>(now - last).count();
  last = now;
  return std::clamp(dt, 0.0, 0.1);
}

void Basis(Vec3 &right, Vec3 &up, Vec3 &forward) {
  const double cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const double cy = std::cos(g_yaw), sy = std::sin(g_yaw);
  forward = {sy * cp, sp, cy * cp};
  right = {cy, 0.0, -sy};
  up = {forward.y * right.z - forward.z * right.y, forward.z * right.x - forward.x * right.z,
        forward.x * right.y - forward.y * right.x};
}

void SeedFrom(const float m[16]) {
  g_pos = {m[12], m[13], m[14]};
  const double fx = m[8], fy = m[9], fz = m[10];
  const double len = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (len > 1e-6) {
    g_pitch = std::clamp(std::asin(std::clamp(fy / len, -1.0, 1.0)), -kPitchLimit, kPitchLimit);
    g_yaw = std::atan2(fx / len, fz / len);
  }
}

void RefreshSaved(Adopted &a) {
  const auto *m = eot::mem::at<eot::be<float>>(a.camera + kWorldMatrix);
  if (!m)
    return;
  float live[16];
  for (int i = 0; i < 16; ++i)
    live[i] = m[i];
  if (a.ever_written && std::memcmp(live, a.written, sizeof(live)) == 0)
    return;
  std::memcpy(a.saved, live, sizeof(live));
}

void BuildRows(float rows[16]) {
  Vec3 right, up, forward;
  Basis(right, up, forward);
  const float built[16] = {
      static_cast<float>(right.x),   static_cast<float>(right.y),
      static_cast<float>(right.z),   0.0f,
      static_cast<float>(up.x),      static_cast<float>(up.y),
      static_cast<float>(up.z),      0.0f,
      static_cast<float>(forward.x), static_cast<float>(forward.y),
      static_cast<float>(forward.z), 0.0f,
      static_cast<float>(g_pos.x),   static_cast<float>(g_pos.y),
      static_cast<float>(g_pos.z),   1.0f,
  };
  std::memcpy(rows, built, sizeof(built));
}

void WriteWorld(uint32_t camera, Adopted *note, const float rows[16]) {
  auto *m = camera ? eot::mem::at<eot::be<float>>(camera + kWorldMatrix) : nullptr;
  if (!m)
    return;
  for (int i = 0; i < 16; ++i)
    m[i] = rows[i];
  if (note) {
    std::memcpy(note->written, rows, sizeof(float) * 16);
    note->ever_written = true;
  }
}

void FrameRows(float rows[16]) {
  if (g_frame_rows_valid)
    std::memcpy(rows, g_frame_rows, sizeof(float) * 16);
  else
    BuildRows(rows);
}

}

namespace eot::debug {

bool FreecamActive() { return g_active; }

bool FreecamDrawTwoSided() { return g_active && REXCVAR_GET(eot_freecam_two_sided); }

bool FreecamReadout(float &x, float &y, float &z, float &yaw_degrees, float &pitch_degrees,
                    double &speed) {
  if (!g_active)
    return false;
  x = static_cast<float>(g_pos.x);
  y = static_cast<float>(g_pos.y);
  z = static_cast<float>(g_pos.z);
  yaw_degrees = static_cast<float>(g_yaw * (180.0 / kPi));
  pitch_degrees = static_cast<float>(g_pitch * (180.0 / kPi));
  speed = g_speed_shown;
  return true;
}

void FreecamTick() {
  const bool want = REXCVAR_GET(eot_freecam);

  {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    ++g_frame;
    ExpireCameras();
  }

  if (want != g_active) {
    g_active = want;
    g_need_seed = want;
    {
      std::lock_guard<std::mutex> lock(g_cams_mutex);
      g_frame_rows_valid = false;
    }
    eot::controller::MouseGrabForDebug(want);
    return;
  }
  if (!g_active || g_need_seed || !WindowHasFocus())
    return;

  if (KeyDown(Key::R)) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    for (int i = 0; i < g_cam_count; ++i) {
      if (!Fresh(g_cams[i]))
        continue;
      SeedFrom(g_cams[i].saved);
      break;
    }
    return;
  }

  const double dt = HostDelta();
  if (dt <= 0.0)
    return;

  float mdx = 0.0f, mdy = 0.0f;
  int notches = 0;
  eot::controller::MouseTakeForDebug(&mdx, &mdy, &notches);
  double base = REXCVAR_GET(eot_freecam_speed);
  if (notches) {
    base = std::clamp(base * std::pow(1.25, notches), 0.05, 5000.0);
    REXCVAR_SET(eot_freecam_speed, base);
    g_speed_shown = base;
  }

  double speed = base;
  if (KeyDown(Key::Shift))
    speed *= 5.0;
  if (KeyDown(Key::Control))
    speed /= 5.0;
  g_speed_shown = speed;

  const double per_pixel = REXCVAR_GET(eot_freecam_mouse_speed) * (kPi / 180.0);
  const double turn = REXCVAR_GET(eot_freecam_turn_speed) * (kPi / 180.0) * dt;
  g_yaw += Axis(Key::Right, Key::Left) * turn + mdx * per_pixel;
  g_pitch = std::clamp(g_pitch + Axis(Key::Up, Key::Down) * turn - mdy * per_pixel, -kPitchLimit,
                       kPitchLimit);

  Vec3 right, up, forward;
  Basis(right, up, forward);

  const double move = speed * dt;
  const double f = Axis(Key::W, Key::S) * move;
  const double s = Axis(Key::D, Key::A) * move;
  const double u = Axis(Key::E, Key::Q) * move;

  g_pos.x += forward.x * f + right.x * s;
  g_pos.y += forward.y * f + right.y * s + u;
  g_pos.z += forward.z * f + right.z * s;
}

}

REX_HOOK_RAW(eot_Renderer_Present) {
  if (g_active) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    BuildRows(g_frame_rows);
    g_frame_rows_valid = true;
    for (int i = 0; i < kCameraListEntries; ++i) {
      const uint32_t cam = eot::mem::load<uint32_t>(kCameraList + i * 4);
      if (!cam)
        continue;
      const uint32_t flags = eot::mem::load<uint32_t>(cam + kCameraFlags);
      if (!(flags & kFlagPublished) || (flags & kFlagSkip))
        continue;
      Adopted *known = FindCamera(cam);
      if (known && Fresh(*known))
        WriteWorld(cam, known, g_frame_rows);
    }
  }
  __imp__eot_Renderer_Present(ctx, base);
}

REX_HOOK_RAW(eot_Renderer_SetupCamera) {
  if (g_active) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    BuildRows(g_frame_rows);
    g_frame_rows_valid = true;
    for (int i = 0; i < g_cam_count; ++i) {
      if (!Fresh(g_cams[i]))
        continue;
      RefreshSaved(g_cams[i]);
      WriteWorld(g_cams[i].camera, &g_cams[i], g_frame_rows);
    }
  }
  __imp__eot_Renderer_SetupCamera(ctx, base);
}

REX_HOOK_RAW(eot_GRMainCamera_ViewBegin) {
  const uint32_t owner = eot::mem::load<uint32_t>(ctx.r4.u32 + kCommandCameraOwner);
  const uint32_t camera =
      owner ? owner : eot::mem::load<uint32_t>(ctx.r3.u32 + kActiveCameraRecord);

  if (g_active && camera) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    Adopted *known = FindCamera(camera);
    if (known) {
      known->last_seen = g_frame;
      RefreshSaved(*known);
    } else if (g_cam_count < kMaxCameras) {
      if (auto *seed = eot::mem::at<eot::be<float>>(camera + kWorldMatrix)) {
        Adopted &slot = g_cams[g_cam_count++];
        slot.camera = camera;
        slot.last_seen = g_frame;
        for (int i = 0; i < 16; ++i)
          slot.saved[i] = seed[i];

        known = &slot;
        if (g_need_seed) {
          g_need_seed = false;
          SeedFrom(slot.saved);
          BuildRows(g_frame_rows);
          g_frame_rows_valid = true;
        }
        EOT_INFO("[freecam] camera {} is 0x{:08X}, at ({:.1f}, {:.1f}, {:.1f})", g_cam_count,
                 camera, slot.saved[12], slot.saved[13], slot.saved[14]);
      }
    }

    float rows[16];
    FrameRows(rows);
    WriteWorld(camera, known, rows);
  } else if (g_cam_count > 0) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    int restored = 0;
    for (int i = 0; i < g_cam_count; ++i) {
      if (!Fresh(g_cams[i]))
        continue;
      if (auto *m = eot::mem::at<eot::be<float>>(g_cams[i].camera + kWorldMatrix)) {
        for (int j = 0; j < 16; ++j)
          m[j] = g_cams[i].saved[j];
        ++restored;
      }
    }
    EOT_INFO("[freecam] released {} of {} camera(s)", restored, g_cam_count);
    g_cam_count = 0;
    g_frame_rows_valid = false;
  }

  __imp__eot_GRMainCamera_ViewBegin(ctx, base);
}

REX_EXTERN(__imp__eot_GLAPIEngine_SetTimeScale);
REX_EXTERN(__imp__eot_HUDWindow_DrawTree);
REX_EXTERN(__imp__eot_Engine_UpdateInput);

REXCVAR_DEFINE_BOOL(eot_debug_pause, false, "EdgeOfTime/Debug", "Freeze the scene");

REXCVAR_DEFINE_BOOL(eot_debug_pause_hide_hud, true, "EdgeOfTime/Debug", "Hide HUD while frozen");

namespace {

constexpr uint32_t kTimeScale = 0x824E56D4;

constexpr uint32_t kInputMask = 0x824C7A4C;

uint32_t g_saved_input_mask = 0;
bool g_input_held = false;

constexpr float kFrozenScale = 1.0e-6f;

float g_game_scale = 1.0f;

std::atomic<bool> g_frozen{false};

std::atomic<int> g_steps{0};

// F1: the whole HUD tree off, frozen or not. A flag, not a cvar, so a hidden HUD
// is never saved into the profile.
std::atomic<bool> g_hud_off{false};

bool Sane(float scale) { return scale > 0.0f && std::isfinite(scale); }

float LoadScale() { return std::bit_cast<float>(eot::mem::load<uint32_t>(kTimeScale)); }

void StoreScale(float scale) {
  eot::mem::store<uint32_t>(kTimeScale, std::bit_cast<uint32_t>(scale));
}

bool HudHidden() {
  if (g_hud_off.load(std::memory_order_relaxed))
    return true;
  return g_frozen.load(std::memory_order_relaxed) && REXCVAR_GET(eot_debug_pause_hide_hud);
}

bool InputBlocked() {
  if (eot::debug::InputScriptHoldsInput())
    return false;
  return g_frozen.load(std::memory_order_relaxed) || eot::debug::FreecamActive();
}

}

namespace eot::debug {

bool ScenePauseActive() { return g_frozen.load(std::memory_order_relaxed); }

void ScenePauseStep() {
  if (!g_frozen.load(std::memory_order_relaxed))
    return;
  g_steps.fetch_add(1, std::memory_order_relaxed);
}

int ScenePauseStepsPending() { return g_steps.load(std::memory_order_relaxed); }

bool ToggleHud() {
  const bool off = !g_hud_off.load(std::memory_order_relaxed);
  g_hud_off.store(off, std::memory_order_relaxed);
  return off;
}

void ScenePauseTick() {
  const bool want = REXCVAR_GET(eot_debug_pause);
  bool simulating = true;

  if (want) {
    g_frozen.store(true, std::memory_order_relaxed);

    int owed = g_steps.load(std::memory_order_relaxed);
    while (owed > 0 && !g_steps.compare_exchange_weak(owed, owed - 1, std::memory_order_relaxed)) {
    }
    simulating = owed > 0;
    StoreScale(simulating ? g_game_scale : kFrozenScale);
  } else {
    g_steps.store(0, std::memory_order_relaxed);

    if (g_frozen.exchange(false, std::memory_order_relaxed)) {
      StoreScale(g_game_scale);
    } else {
      const float live = LoadScale();
      if (Sane(live))
        g_game_scale = live;
    }
  }

  eot::debug::InputScriptTick(simulating);
}

}

REX_HOOK_RAW(eot_GLAPIEngine_SetTimeScale) {
  const double requested = ctx.f1.f64;
  if (requested > 0.0 && std::isfinite(requested))
    g_game_scale = static_cast<float>(requested);

  if (g_frozen.load(std::memory_order_relaxed))
    return;
  __imp__eot_GLAPIEngine_SetTimeScale(ctx, base);
}

REX_HOOK_RAW(eot_HUDWindow_DrawTree) {
  if (HudHidden()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__eot_HUDWindow_DrawTree(ctx, base);
}

REX_HOOK_RAW(eot_Engine_UpdateInput) {
  __imp__eot_Engine_UpdateInput(ctx, base);

  if (InputBlocked()) {
    g_saved_input_mask = eot::mem::load<uint32_t>(kInputMask);
    g_input_held = true;
    eot::mem::store<uint32_t>(kInputMask, 0u);
    return;
  }

  if (g_input_held) {
    g_input_held = false;
    eot::mem::store<uint32_t>(kInputMask, g_saved_input_mask);
  }
}

REX_EXTERN(__imp__eot_Renderer_UpdateViewMatrix);

REXCVAR_DEFINE_STRING(
    eot_occlusion, "auto", "EdgeOfTime/Debug", "Occlusion culling mode");

namespace {

constexpr uint32_t kOccluderList = 0x824D7534;
constexpr uint32_t kCapacity = 4;
constexpr uint32_t kSlotsUsed = 8;
constexpr uint32_t kLiveCount = 12;

std::atomic<bool> g_suppressed{false};

bool Wanted() {
  const std::string mode = rex::cvar::GetFlagByName("eot_occlusion");
  if (mode == "off")
    return true;
  if (mode == "on")
    return false;
  return eot::debug::FreecamActive();
}

}

namespace eot::debug {

bool OcclusionSuppressed() { return g_suppressed.load(std::memory_order_relaxed); }

}

REX_HOOK_RAW(eot_Renderer_UpdateViewMatrix) {
  const bool hold = Wanted();
  if (hold != g_suppressed.exchange(hold, std::memory_order_relaxed))
    EOT_INFO("[occlusion] occluder culling {}", hold ? "held off" : "back on");

  if (!hold) {
    __imp__eot_Renderer_UpdateViewMatrix(ctx, base);
    return;
  }

  const uint32_t capacity = eot::mem::load<uint32_t>(kOccluderList + kCapacity);
  const uint32_t slots = eot::mem::load<uint32_t>(kOccluderList + kSlotsUsed);
  const uint32_t live = eot::mem::load<uint32_t>(kOccluderList + kLiveCount);
  eot::mem::store<uint32_t>(kOccluderList + kCapacity, 0u);
  eot::mem::store<uint32_t>(kOccluderList + kSlotsUsed, 0u);
  eot::mem::store<uint32_t>(kOccluderList + kLiveCount, 0u);

  __imp__eot_Renderer_UpdateViewMatrix(ctx, base);

  eot::mem::store<uint32_t>(kOccluderList + kCapacity, capacity);
  eot::mem::store<uint32_t>(kOccluderList + kSlotsUsed, slots);
  eot::mem::store<uint32_t>(kOccluderList + kLiveCount, live);
}

REX_EXTERN(__imp__eot_RZHAnim_LoadDiscardableData); // (animation r3, chunk r4, stream r5)

namespace {
using Clock = std::chrono::steady_clock;

constexpr auto kBurstGap = std::chrono::milliseconds(500);

struct Burst {
  std::mutex lock;
  Clock::time_point first{};
  Clock::time_point last{};
  uint32_t count = 0;
  int64_t slowest_us = 0;
};
Burst g_burst;

void Flush(Burst &burst) {
  if (!burst.count)
    return;
  const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(burst.last - burst.first);
  EOT_DEBUG("[anim] {} animation(s) loaded over {} ms, ending {} ms ago (slowest {} us)", burst.count, span.count(),
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - burst.last).count(),
            burst.slowest_us);
  burst.count = 0;
  burst.slowest_us = 0;
}
}

REX_HOOK_RAW(eot_RZHAnim_LoadDiscardableData) {
  const auto start = Clock::now();
  __imp__eot_RZHAnim_LoadDiscardableData(ctx, base);
  const auto end = Clock::now();
  std::lock_guard<std::mutex> guard(g_burst.lock);
  if (g_burst.count && start - g_burst.last > kBurstGap)
    Flush(g_burst);
  if (!g_burst.count)
    g_burst.first = start;
  g_burst.last = end;
  ++g_burst.count;
  g_burst.slowest_us =
      std::max<int64_t>(g_burst.slowest_us, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

REXCVAR_DEFINE_STRING(
    eot_debug_script, "", "EdgeOfTime/Debug", "Input script to replay");

namespace {

using eot::controller::PadInput;
using eot::controller::RawPad;

constexpr size_t kMaxSteps = 64;
constexpr uint32_t kMaxFrames = 3600;
constexpr uint32_t kDefaultFrames = 2;
constexpr int16_t kStickFull = 32767;
constexpr uint8_t kTriggerFull = 255;

struct Step {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint32_t frames = kDefaultFrames;
  std::string label;
};

std::mutex g_mutex;
std::vector<Step> g_script_steps;
size_t g_at = 0;
uint32_t g_left = 0;
bool g_running = false;
Step g_live;
bool g_live_valid = false;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

bool ParseUint(std::string_view s, uint32_t &out) {
  s = Trim(s);
  if (s.empty())
    return false;
  uint32_t value = 0;
  const auto end = s.data() + s.size();
  const auto result = std::from_chars(s.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end)
    return false;
  out = value;
  return true;
}

bool ParseAxis(std::string_view s, int16_t &out) {
  s = Trim(s);
  if (s.empty())
    return false;
  const std::string text(s);
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (!end || *end != '\0')
    return false;
  out = static_cast<int16_t>(std::clamp(value, -1.0, 1.0) * kStickFull);
  return true;
}

PadInput InputNamed(std::string_view name) {
  name = Trim(name);
  for (const eot::controller::PadAction &action : eot::controller::kPadActions)
    if (name == action.id)
      return action.native;
  return eot::controller::ParsePadInput(name);
}

void PressInto(Step &step, PadInput input) {
  switch (input) {
  case PadInput::LT:
    step.left_trigger = kTriggerFull;
    break;
  case PadInput::RT:
    step.right_trigger = kTriggerFull;
    break;
  default:
    step.buttons |= eot::controller::PadInputBit(input);
    break;
  }
}

bool ParseStep(std::string_view token, Step &step) {
  const size_t colon = token.find(':');
  const std::string_view head = Trim(token.substr(0, colon));
  std::string_view rest = colon == std::string_view::npos ? std::string_view{} : token.substr(colon + 1);

  if (head == "wait") {
    step.frames = kDefaultFrames;
    if (!rest.empty() && !ParseUint(rest, step.frames))
      return false;
    step.label = "wait";
    return true;
  }

  if (head == "stick" || head == "rstick") {
    const size_t frames_at = rest.find(':');
    std::string_view pair = rest.substr(0, frames_at);
    if (frames_at != std::string_view::npos && !ParseUint(rest.substr(frames_at + 1), step.frames))
      return false;
    const size_t comma = pair.find(',');
    if (comma == std::string_view::npos)
      return false;
    int16_t x = 0, y = 0;
    if (!ParseAxis(pair.substr(0, comma), x) || !ParseAxis(pair.substr(comma + 1), y))
      return false;
    if (head == "stick") {
      step.lx = x;
      step.ly = y;
    } else {
      step.rx = x;
      step.ry = y;
    }
    step.label = std::string(head);
    return true;
  }

  if (!rest.empty() && !ParseUint(rest, step.frames))
    return false;
  std::string_view names = head;
  bool any = false;
  while (!names.empty()) {
    const size_t plus = names.find('+');
    const std::string_view one = Trim(names.substr(0, plus));
    const PadInput input = InputNamed(one);
    if (input == PadInput::None)
      return false;
    PressInto(step, input);
    step.label += std::string(step.label.empty() ? "" : "+") + std::string(one);
    any = true;
    if (plus == std::string_view::npos)
      break;
    names.remove_prefix(plus + 1);
  }
  return any;
}

void Stop() {
  g_running = false;
  g_live_valid = false;
  g_at = 0;
  g_left = 0;
}

}

namespace eot::debug {

uint32_t InputScriptSet(std::string_view script) {
  std::vector<Step> steps;
  std::string_view rest = script;
  while (!rest.empty()) {
    const size_t at = rest.find_first_of(" \t;");
    const std::string_view token = Trim(rest.substr(0, at));
    rest = at == std::string_view::npos ? std::string_view{} : rest.substr(at + 1);
    if (token.empty())
      continue;
    if (steps.size() >= kMaxSteps) {
      EOT_WARN("[script] more than {} steps; the rest is ignored", kMaxSteps);
      break;
    }
    Step step;
    if (!ParseStep(token, step)) {
      EOT_WARN("[script] cannot read '{}'; skipped", std::string(token));
      continue;
    }
    step.frames = std::clamp(step.frames, 1u, kMaxFrames);
    steps.push_back(std::move(step));
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  Stop();
  g_script_steps = std::move(steps);
  if (g_script_steps.empty()) {
    if (!Trim(script).empty())
      EOT_WARN("[script] nothing to run");
    return 0;
  }
  g_running = true;
  uint32_t frames = 0;
  std::string listing;
  for (const Step &step : g_script_steps) {
    frames += step.frames;
    listing += std::string(listing.empty() ? "" : " ") + step.label + ":" + std::to_string(step.frames);
  }
  EOT_INFO("[script] {} step(s), {} frame(s): {}", g_script_steps.size(), frames, listing);
  return static_cast<uint32_t>(g_script_steps.size());
}

void InputScriptReplay() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_script_steps.empty()) {
    EOT_INFO("[script] nothing queued (set eot_debug_script)");
    return;
  }
  g_at = 0;
  g_left = 0;
  g_live_valid = false;
  g_running = true;
  EOT_INFO("[script] armed, {} step(s)", g_script_steps.size());
}

void InputScriptTick(bool simulating) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_live_valid = false;
  if (!g_running || !simulating)
    return;

  if (g_left == 0) {
    if (g_at >= g_script_steps.size()) {
      EOT_INFO("[script] done");
      Stop();
      return;
    }
    g_left = g_script_steps[g_at].frames;
  }

  g_live = g_script_steps[g_at];
  g_live_valid = true;
  if (--g_left == 0)
    ++g_at;
}

bool InputScriptPad(controller::RawPad &pad) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_live_valid)
    return false;
  pad.buttons = g_live.buttons;
  pad.left_trigger = g_live.left_trigger;
  pad.right_trigger = g_live.right_trigger;
  pad.thumb_lx = g_live.lx;
  pad.thumb_ly = g_live.ly;
  pad.thumb_rx = g_live.rx;
  pad.thumb_ry = g_live.ry;
  return true;
}

bool InputScriptHoldsInput() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_live_valid;
}

std::string InputScriptReadout() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_running || g_script_steps.empty())
    return {};
  if (g_left)
    return std::format("script  step {}/{}  {}  {} frame(s) left", g_at + 1, g_script_steps.size(),
                       g_script_steps[g_at].label, g_left);
  if (g_at < g_script_steps.size())
    return std::format("script  step {}/{} next  {}  {} frame(s)", g_at + 1, g_script_steps.size(),
                       g_script_steps[g_at].label, g_script_steps[g_at].frames);
  return std::format("script  {} step(s) done", g_script_steps.size());
}

}
