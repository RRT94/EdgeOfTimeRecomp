// gamelogic/input/input.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/export.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/menu_common.h"

REX_EXTERN(__imp__eot_Camera3rd_ReadStick);

REXCVAR_DEFINE_BOOL(eot_mouse_look, true, "EdgeOfTime/Input", "Mouse turns the camera");
REXCVAR_DEFINE_DOUBLE(eot_mouse_look_degrees, 0.1, "EdgeOfTime/Input", "Mouse look turn speed");

namespace {

constexpr uint32_t kPitchMin = 484;
constexpr uint32_t kPitchMax = 488;
constexpr uint32_t kHold = 524;
constexpr uint32_t kYaw = 604;
constexpr uint32_t kPitch = 608;
constexpr uint32_t kHoldTimer = 640;
constexpr uint32_t kTransition = 672;
constexpr uint32_t kVelYaw = 724;
constexpr uint32_t kVelPitch = 728;

constexpr uint32_t kActionTable = 0x824C7A24;
constexpr uint32_t kActionCamX = 34;
constexpr uint32_t kActionCamY = 35;
constexpr uint32_t kInvertY = 0x883CA0F5;
constexpr uint32_t kInvertX = 0x883CA0F6;

constexpr float kPi = 3.14159265f;

using TakeFn = void (*)(float *, float *);
std::atomic<TakeFn> g_take{nullptr};
std::atomic<bool> g_resolved{false};
std::atomic<bool> g_enabled{true};
std::atomic<float> g_radians_per_pixel{0.03f * kPi / 180.0f};
std::atomic<bool> g_callbacks{false};
bool g_announced = false;

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

void ReadSettings() {
  g_enabled.store(rex::cvar::Query<bool>("eot_mouse_look"), std::memory_order_relaxed);
  const float degrees = static_cast<float>(std::atof(rex::cvar::GetFlagByName("eot_mouse_look_degrees").c_str()));
  const float sensitivity = static_cast<float>(std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str()));
  g_radians_per_pixel.store(degrees * (kPi / 180.0f) * (sensitivity > 0.0f ? sensitivity : 1.0f),
                            std::memory_order_relaxed);
}

TakeFn Resolve() {
  if (!g_resolved.exchange(true)) {
    g_take.store(reinterpret_cast<TakeFn>(eot::HostEntryPoint("eot_mouse_take")), std::memory_order_release);
    if (!g_take.load())
      EOT_WARN("[camera] no eot_mouse_take in the host; mouse look stays on the stick");
    for (const char *name : {"eot_mouse_look", "eot_mouse_look_degrees", "mnk_sensitivity"})
      rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) { ReadSettings(); });
    ReadSettings();
  }
  return g_take.load(std::memory_order_acquire);
}

}

REX_HOOK_RAW(eot_Camera3rd_ReadStick) {
  const uint32_t cam = ctx.r3.u32;
  const TakeFn take = Resolve();
  float dx = 0.0f, dy = 0.0f;
  if (take && g_enabled.load(std::memory_order_relaxed))
    take(&dx, &dy);
  const bool moved = dx != 0.0f || dy != 0.0f;

  const uint32_t actions = eot::mem::load<uint32_t>(kActionTable);
  float x = 0.0f, y = 0.0f;
  if (moved && actions) {
    x = LoadF(actions + kActionCamX * 4);
    y = LoadF(actions + kActionCamY * 4);
    StoreF(actions + kActionCamX * 4, 0.0f);
    StoreF(actions + kActionCamY * 4, 0.0f);
  }
  __imp__eot_Camera3rd_ReadStick(ctx, base);
  if (!moved)
    return;
  if (actions) {
    StoreF(actions + kActionCamX * 4, x);
    StoreF(actions + kActionCamY * 4, y);
  }
  if (!g_announced) {
    g_announced = true;
    EOT_INFO("[camera] mouse look: the motion turns the camera directly");
  }

  const float k = g_radians_per_pixel.load(std::memory_order_relaxed);
  const float sx = eot::mem::load<uint8_t>(kInvertX) ? 1.0f : -1.0f;
  const float sy = eot::mem::load<uint8_t>(kInvertY) ? 1.0f : -1.0f;
  float yaw = LoadF(cam + kYaw) + sx * dx * k;
  float pitch = LoadF(cam + kPitch) + sy * dy * k;
  while (yaw > kPi)
    yaw -= 2.0f * kPi;
  while (yaw < -kPi)
    yaw += 2.0f * kPi;
  pitch = std::clamp(pitch, LoadF(cam + kPitchMin), LoadF(cam + kPitchMax));
  StoreF(cam + kYaw, yaw);
  StoreF(cam + kPitch, pitch);
  StoreF(cam + kHoldTimer, LoadF(cam + kHold));
  eot::mem::store<uint8_t>(cam + kTransition, 0);
  StoreF(cam + kVelYaw, 0.0f);
  StoreF(cam + kVelPitch, 0.0f);
  ctx.r3.u32 = 1;
}

REX_EXTERN(__imp__eot_ButtonMash_Update); // (helper r3, dt f1): a frame of a mash
REX_EXTERN(__imp__eot_BaseHeroSM_HandleCombatMessages); // (hero r3, message r4, payload r5) -> handled

namespace {

constexpr uint32_t kSignedInUser = 0x883CA2C0;
constexpr uint32_t kFinishInReach = 0x8726681A;
constexpr uint32_t kHeroFinishFlags = 14476;
constexpr uint32_t kLogicInputs = 80;
constexpr uint32_t kRecordSize = 48;
constexpr uint32_t kMaxSources = 4;

constexpr uint16_t kHardwareButton[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200};

uint16_t ButtonsFor(uint32_t input) {
  if (input >= kLogicInputs)
    return 0;
  const uint32_t users = eot::mem::load<uint32_t>(kSignedInUser);
  const uint32_t table = users ? eot::mem::load<uint32_t>(users + 12) : 0;
  if (!table)
    return 0;
  const uint32_t record = table + kRecordSize * input;
  const uint32_t count = eot::mem::load<uint8_t>(record + 47);
  uint16_t buttons = 0;
  for (uint32_t i = 0; i < count && i < kMaxSources; ++i) {
    const uint32_t source = eot::mem::load<uint32_t>(record + 8 * i);
    if (!source || eot::mem::load<uint32_t>(source) != 1)
      continue;
    const uint32_t hardware = eot::mem::load<uint32_t>(source + 4);
    if (hardware < sizeof(kHardwareButton) / sizeof(kHardwareButton[0]))
      buttons |= kHardwareButton[hardware];
  }
  return buttons;
}

}

REX_HOOK_RAW(eot_ButtonMash_Update) {
  const uint32_t helper = ctx.r3.u32;
  __imp__eot_ButtonMash_Update(ctx, base);
  if (eot::mem::load<uint8_t>(helper + 52) & 0x80)
    return;
  const uint16_t buttons = ButtonsFor(eot::mem::load<uint32_t>(helper + 48));
  if (!buttons)
    return;
  if (const auto note = eot::ui::binds::Api().mash_prompt)
    note(buttons);
}

REX_HOOK_RAW(eot_BaseHeroSM_HandleCombatMessages) {
  const uint32_t hero = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  __imp__eot_BaseHeroSM_HandleCombatMessages(ctx, base);
  if (message != kFinishInReach || !hero)
    return;
  if (const auto note = eot::ui::binds::Api().finish_prompt)
    note((eot::mem::load<uint8_t>(hero + kHeroFinishFlags) & 1) ? 1 : 0);
}
