// goliath/controller/pad_remap.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/controller/pad_remap.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/kernel/xam/module.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/bind_capture.h"
#include "goliath/controller/mouse_input.h"
#include "goliath/debug/freecam.h"

REX_EXTERN(__imp__eot_XInputGetState);

#define EOT_PAD "Input/Keybinds/Edge of Time (controller)"

REXCVAR_DEFINE_STRING(eot_pad_jump, "A", EOT_PAD, "Jump / Web Jump");
REXCVAR_DEFINE_STRING(eot_pad_web, "B", EOT_PAD, "Web Shot / Interact");
REXCVAR_DEFINE_STRING(eot_pad_light_attack, "X", EOT_PAD, "Melee attack button");
REXCVAR_DEFINE_STRING(eot_pad_heavy_attack, "Y", EOT_PAD, "Ranged Attack / Air Launcher");
REXCVAR_DEFINE_STRING(eot_pad_grab, "RB", EOT_PAD, "Grab or strike button");
REXCVAR_DEFINE_STRING(eot_pad_special_attack, "LB", EOT_PAD, "Throw object button");
REXCVAR_DEFINE_STRING(eot_pad_web_swing, "RT", EOT_PAD, "Web Zip / Web Swing");
REXCVAR_DEFINE_STRING(eot_pad_hyper_sense, "LT", EOT_PAD, "Special ability button");
REXCVAR_DEFINE_STRING(eot_pad_spider_sense, "Up", EOT_PAD, "Spider-Sense / Accelerated Vision");
REXCVAR_DEFINE_STRING(eot_pad_left_stick_click, "LS", EOT_PAD, "Stick to wall button");
REXCVAR_DEFINE_STRING(eot_pad_right_stick_click, "RS", EOT_PAD, "Center camera button");
REXCVAR_DEFINE_STRING(eot_pad_pause, "Start", EOT_PAD, "Pause menu button");
REXCVAR_DEFINE_STRING(eot_pad_upgrades, "Back", EOT_PAD, "Upgrades menu button");
REXCVAR_DEFINE_STRING(eot_pad_time_paradox, "", EOT_PAD, "Time Stop button");
REXCVAR_DEFINE_STRING(eot_pad_sticks, "normal", EOT_PAD, "Swap the sticks");

namespace eot::controller {

namespace {

constexpr uint8_t kTriggerPressed = 30;

constexpr const PadAction *kActions = kPadActions;

struct Table {
  std::array<PadInput, kPadActionCount> physical{};
  uint32_t claimed = 0;
  uint32_t native = 0;
  bool swapped = false;
  bool identity = true;
};
Table g_table;
std::mutex g_table_mutex;
std::atomic<bool> g_dirty{true};
uint32_t g_polls = 0;
constexpr uint32_t kRecheckPolls = 120;

constexpr uint32_t Bit(PadInput input) { return 1u << static_cast<uint32_t>(input); }

Table Build() {
  Table t;
  for (uint32_t i = 0; i < kPadActionCount; ++i) {
    const PadInput physical = PhysicalFor(kActions[i]);
    t.physical[i] = physical;
    t.claimed |= Bit(physical);
    t.native |= Bit(kActions[i].native);
    if (physical != kActions[i].native)
      t.identity = false;
  }
  t.swapped = SticksSwapped();
  if (t.swapped)
    t.identity = false;
  return t;
}

bool PressedIn(const RawPad &pad, PadInput input) {
  switch (input) {
  case PadInput::LT:
    return pad.left_trigger > kTriggerPressed;
  case PadInput::RT:
    return pad.right_trigger > kTriggerPressed;
  case PadInput::LSRS:
    return (pad.buttons & PadInputBit(PadInput::LSRS)) == PadInputBit(PadInput::LSRS);
  case PadInput::None:
    return false;
  default:
    return (pad.buttons & PadInputBit(input)) != 0;
  }
}

uint8_t TriggerFrom(const RawPad &pad, PadInput input) {
  switch (input) {
  case PadInput::LT:
    return pad.left_trigger;
  case PadInput::RT:
    return pad.right_trigger;
  default:
    return PressedIn(pad, input) ? 255 : 0;
  }
}

void Press(RawPad &out, PadInput native, const RawPad &in, PadInput physical) {
  switch (native) {
  case PadInput::LT:
    out.left_trigger = std::max(out.left_trigger, TriggerFrom(in, physical));
    break;
  case PadInput::RT:
    out.right_trigger = std::max(out.right_trigger, TriggerFrom(in, physical));
    break;
  default:
    if (PressedIn(in, physical))
      out.buttons |= PadInputBit(native);
    break;
  }
}

}

PadInput PhysicalFor(const PadAction &action) {
  const PadInput bound = ParsePadInput(rex::cvar::GetFlagByName(action.pad_cvar));
  return bound == PadInput::None ? action.native : bound;
}

bool SticksSwapped() { return rex::cvar::GetFlagByName(kPadSticksCvar) == "swapped"; }

void PadRemapChanged() { g_dirty.store(true, std::memory_order_release); }

bool PadRemapActive() {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  return !g_table.identity;
}

void RemapPad(RawPad &pad) {
  Table t;
  {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    t = g_table;
  }
  if (t.identity)
    return;
  RawPad out = pad;
  out.buttons = 0;
  out.left_trigger = 0;
  out.right_trigger = 0;
  for (uint32_t i = 0; i < kPadActionCount; ++i)
    Press(out, kActions[i].native, pad, t.physical[i]);
  for (const PadInputInfo &i : kPadInputs) {
    if (i.input == PadInput::None || (t.claimed & Bit(i.input)) || (t.native & Bit(i.input)))
      continue;
    if (i.input == PadInput::LT)
      out.left_trigger = std::max(out.left_trigger, pad.left_trigger);
    else if (i.input == PadInput::RT)
      out.right_trigger = std::max(out.right_trigger, pad.right_trigger);
    else
      out.buttons |= pad.buttons & i.bit;
  }
  out.buttons |= pad.buttons & rex::input::X_INPUT_GAMEPAD_GUIDE;
  if (t.swapped && ActivePad() != PadBrand::Keyboard) {
    std::swap(out.thumb_lx, out.thumb_rx);
    std::swap(out.thumb_ly, out.thumb_ry);
  }
  pad = out;
}

namespace {

// Keys are bound per action and pressed here: one key can press a chord, and a modifier bound
// as a key of its own does not silence the other binds.
constexpr size_t kKeysZOrder = 1;
constexpr uint8_t kModShift = 1, kModCtrl = 2, kModAlt = 4;
constexpr int16_t kStickInUse = 12000;
constexpr float kMeantMotion = 24.0f;
constexpr auto kMotionWindow = std::chrono::milliseconds(500);
constexpr auto kMouseLookFresh = std::chrono::milliseconds(150);

using Clock = std::chrono::steady_clock;

int64_t Now() { return Clock::now().time_since_epoch().count(); }

int64_t Ticks(Clock::duration d) { return d.count(); }

uint16_t KeyIndex(rex::ui::VirtualKey vk) {
  using rex::ui::VirtualKey;
  switch (vk) {
  case VirtualKey::kLShift:
  case VirtualKey::kRShift:
    return static_cast<uint16_t>(VirtualKey::kShift);
  case VirtualKey::kLControl:
  case VirtualKey::kRControl:
    return static_cast<uint16_t>(VirtualKey::kControl);
  case VirtualKey::kLMenu:
  case VirtualKey::kRMenu:
    return static_cast<uint16_t>(VirtualKey::kMenu);
  default:
    return static_cast<uint16_t>(vk);
  }
}

uint16_t MouseKey(rex::ui::MouseEvent::Button button) {
  switch (button) {
  case rex::ui::MouseEvent::Button::kLeft:
    return static_cast<uint16_t>(rex::ui::VirtualKey::kLButton);
  case rex::ui::MouseEvent::Button::kRight:
    return static_cast<uint16_t>(rex::ui::VirtualKey::kRButton);
  case rex::ui::MouseEvent::Button::kMiddle:
    return static_cast<uint16_t>(rex::ui::VirtualKey::kMButton);
  default:
    return 0;
  }
}

using Keys = std::array<bool, 256>;

class KeyState final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || window_)
      return;
    window_ = window;
    window->AddInputListener(this, kKeysZOrder);
    window->AddListener(this);
  }

  Keys Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return down_;
  }

  bool Focused() const { return focused_.load(std::memory_order_acquire); }
  int64_t LastUsed() const { return last_used_.load(std::memory_order_acquire); }
  bool MouseLookFresh() const {
    return Now() - last_motion_.load(std::memory_order_acquire) <= Ticks(kMouseLookFresh);
  }

  void OnKeyDown(rex::ui::KeyEvent &e) override { Set(KeyIndex(e.virtual_key()), true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Set(KeyIndex(e.virtual_key()), false); }
  void OnMouseDown(rex::ui::MouseEvent &e) override { Set(MouseKey(e.button()), true); }
  void OnMouseUp(rex::ui::MouseEvent &e) override { Set(MouseKey(e.button()), false); }

  void OnMouseMove(rex::ui::MouseEvent &e) override {
    std::lock_guard<std::mutex> lock(mutex_);
    float motion = std::fabs(e.dx()) + std::fabs(e.dy());
    if (motion == 0.0f && have_position_)
      motion = static_cast<float>(std::abs(e.x() - x_) + std::abs(e.y() - y_));
    x_ = e.x();
    y_ = e.y();
    have_position_ = true;
    if (motion == 0.0f)
      return;
    const int64_t now = Now();
    if (now - last_motion_.load(std::memory_order_relaxed) > Ticks(kMotionWindow))
      motion_ = 0.0f;
    motion_ += motion;
    last_motion_.store(now, std::memory_order_release);
    if (motion_ >= kMeantMotion)
      last_used_.store(now, std::memory_order_release);
  }

  void OnLostFocus(rex::ui::UISetupEvent &) override {
    focused_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    down_ = {};
    motion_ = 0.0f;
    have_position_ = false;
  }
  void OnGotFocus(rex::ui::UISetupEvent &) override { focused_.store(true, std::memory_order_release); }

  void OnClosing(rex::ui::UIEvent &) override {
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
      window_ = nullptr;
    }
  }

private:
  void Set(uint16_t index, bool down) {
    if (!index || index >= 256)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    down_[index] = down;
    if (down)
      last_used_.store(Now(), std::memory_order_release);
  }

  rex::ui::Window *window_ = nullptr;
  std::mutex mutex_;
  Keys down_{};
  std::atomic<bool> focused_{true};
  std::atomic<int64_t> last_used_{0};
  std::atomic<int64_t> last_motion_{0};
  float motion_ = 0.0f;
  int32_t x_ = 0, y_ = 0;
  bool have_position_ = false;
};
KeyState g_keys;

std::atomic<int64_t> g_pad_used{0};
std::atomic<bool> g_has_pad{false};
std::atomic<PadBrand> g_pad_brand{PadBrand::Unknown};

bool MnkMode() { return rex::cvar::GetFlagByName("mnk_mode") == "true"; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ')
    s.remove_suffix(1);
  return s;
}

uint8_t TakeModifiers(std::string_view &token) {
  uint8_t mods = 0;
  for (;;) {
    const size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0)
      return mods;
    const std::string_view head = token.substr(0, plus);
    if (head == "Shift")
      mods |= kModShift;
    else if (head == "Ctrl" || head == "Control")
      mods |= kModCtrl;
    else if (head == "Alt")
      mods |= kModAlt;
    else
      return mods;
    token.remove_prefix(plus + 1);
  }
}

template <typename F> void ForEachToken(std::string_view value, F &&f) {
  while (!value.empty()) {
    const size_t comma = value.find(',');
    const std::string_view token = Trim(value.substr(0, comma));
    value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
    if (!token.empty())
      f(token);
  }
}

uint8_t BoundModifiers(std::string_view value) {
  uint8_t mods = 0;
  ForEachToken(value, [&](std::string_view token) {
    TakeModifiers(token);
    if (token == "Shift")
      mods |= kModShift;
    else if (token == "Ctrl" || token == "Control")
      mods |= kModCtrl;
    else if (token == "Alt")
      mods |= kModAlt;
  });
  return mods;
}

uint8_t LiveModifiers(const Keys &keys) {
  uint8_t mods = 0;
  if (keys[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (keys[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (keys[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

bool BindPressed(const Keys &keys, std::string_view value, uint8_t live, uint8_t bound) {
  bool pressed = false;
  ForEachToken(value, [&](std::string_view token) {
    if (pressed || TakeModifiers(token) != (live & ~bound))
      return;
    const rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
    const uint16_t index = KeyIndex(vk);
    pressed = vk != rex::ui::VirtualKey::kNone && index < keys.size() && keys[index];
  });
  return pressed;
}

struct KeyboardPad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool left_keys = false, right_keys = false;
};

KeyboardPad ReadKeyboard(const Table &t) {
  KeyboardPad out;
  if (!MnkMode() || !g_keys.Focused() || rex::kernel::xam::xeXamIsUIActive())
    return out;
  const Keys keys = g_keys.Snapshot();

  struct Bind {
    std::string value;
    PadInput input;
  };
  std::vector<Bind> binds;
  binds.reserve(kPadActionCount + 3);
  for (uint32_t i = 0; i < kPadActionCount; ++i)
    binds.push_back({rex::cvar::GetFlagByName(kActions[i].key_cvar), t.physical[i]});
  binds.push_back({rex::cvar::GetFlagByName("eot_key_dpad_down"), PadInput::Down});
  binds.push_back({rex::cvar::GetFlagByName("eot_key_dpad_left"), PadInput::Left});
  binds.push_back({rex::cvar::GetFlagByName("eot_key_dpad_right"), PadInput::Right});
  std::array<std::string, 8> sticks;
  for (size_t i = 0; i < 4; ++i) {
    sticks[i] = rex::cvar::GetFlagByName(kMoveKeyCvars[i]);
    sticks[i + 4] = rex::cvar::GetFlagByName(kLookKeyCvars[i]);
  }

  uint8_t bound = 0;
  for (const Bind &b : binds)
    bound |= BoundModifiers(b.value);
  for (const std::string &v : sticks)
    bound |= BoundModifiers(v);
  const uint8_t live = LiveModifiers(keys);

  for (const Bind &b : binds) {
    if (!BindPressed(keys, b.value, live, bound))
      continue;
    if (b.input == PadInput::LT)
      out.left_trigger = 0xFF;
    else if (b.input == PadInput::RT)
      out.right_trigger = 0xFF;
    else
      out.buttons |= PadInputBit(b.input);
  }

  bool held[8];
  for (size_t i = 0; i < sticks.size(); ++i) {
    held[i] = BindPressed(keys, sticks[i], live, bound);
    out.left_keys = out.left_keys || (i < 4 && BindPressed(keys, sticks[i], 0, 0xFF));
    out.right_keys = out.right_keys || (i >= 4 && BindPressed(keys, sticks[i], 0, 0xFF));
  }
  out.ly = (held[0] ? INT16_MAX : 0) - (held[1] ? INT16_MAX : 0);
  out.lx = (held[3] ? INT16_MAX : 0) - (held[2] ? INT16_MAX : 0);
  out.ry = (held[4] ? INT16_MAX : 0) - (held[5] ? INT16_MAX : 0);
  out.rx = (held[7] ? INT16_MAX : 0) - (held[6] ? INT16_MAX : 0);
  return out;
}

void NotePadUse(const RawPad &pad, const KeyboardPad &keys) {
  bool used = pad.buttons != 0 || pad.left_trigger > kTriggerPressed || pad.right_trigger > kTriggerPressed;
  if (!keys.left_keys && (std::abs(pad.thumb_lx) > kStickInUse || std::abs(pad.thumb_ly) > kStickInUse))
    used = true;
  if (!keys.right_keys && !g_keys.MouseLookFresh() &&
      (std::abs(pad.thumb_rx) > kStickInUse || std::abs(pad.thumb_ry) > kStickInUse))
    used = true;
  if (used)
    g_pad_used.store(Now(), std::memory_order_release);
}

void Merge(int16_t &axis, int32_t keys) {
  if (!keys)
    return;
  const int16_t value = static_cast<int16_t>(std::clamp<int32_t>(keys, -INT16_MAX, INT16_MAX));
  if (std::abs(value) >= std::abs(axis))
    axis = value;
}

void ApplyKeyboard(RawPad &pad, const KeyboardPad &keys) {
  pad.buttons |= keys.buttons;
  pad.left_trigger = std::max(pad.left_trigger, keys.left_trigger);
  pad.right_trigger = std::max(pad.right_trigger, keys.right_trigger);
  Merge(pad.thumb_lx, keys.lx);
  Merge(pad.thumb_ly, keys.ly);
  Merge(pad.thumb_rx, keys.rx);
  Merge(pad.thumb_ry, keys.ry);
}

}

}

REX_HOOK_RAW(eot_XInputGetState) {
  using namespace eot::controller;
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__eot_XInputGetState(ctx, base);
  if (ctx.r3.u32 != 0 || !state)
    return;
  if (g_dirty.exchange(false, std::memory_order_acq_rel) || ++g_polls % kRecheckPolls == 0) {
    Table t = Build();
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_table_mutex);
      changed = t.physical != g_table.physical || t.swapped != g_table.swapped;
      g_table = t;
    }
    if (changed) {
      EOT_DEBUG("[pad] binds {}: {}{}", t.identity ? "native" : "remapped", [&] {
        std::string s;
        for (uint32_t i = 0; i < kPadActionCount; ++i)
          if (t.physical[i] != kActions[i].native)
            s += std::string(s.empty() ? "" : ", ") + kActions[i].id + " on " + PadInputName(t.physical[i]);
        return s.empty() ? std::string("every action on its own button") : s;
      }(), t.swapped ? "; sticks swapped" : "");
    }
  }
  RawPad pad;
  pad.buttons = eot::mem::load<uint16_t>(state + 4);
  pad.left_trigger = eot::mem::load<uint8_t>(state + 6);
  pad.right_trigger = eot::mem::load<uint8_t>(state + 7);
  pad.thumb_lx = eot::mem::load<int16_t>(state + 8);
  pad.thumb_ly = eot::mem::load<int16_t>(state + 10);
  pad.thumb_rx = eot::mem::load<int16_t>(state + 12);
  pad.thumb_ry = eot::mem::load<int16_t>(state + 14);
  if (user == 0) {
    Table t;
    {
      std::lock_guard<std::mutex> lock(g_table_mutex);
      t = g_table;
    }
    const KeyboardPad keys = ReadKeyboard(t);
    NotePadUse(pad, keys);
    ApplyKeyboard(pad, keys);
  }
  const bool swallowed = FilterPadForCapture(pad);
  if (!swallowed) {
    RemapPad(pad);
    ApplyMenuKeys(pad);
  }
  eot::debug::InputScriptPad(pad);
  eot::mem::store<uint16_t>(state + 4, pad.buttons);
  eot::mem::store<uint8_t>(state + 6, pad.left_trigger);
  eot::mem::store<uint8_t>(state + 7, pad.right_trigger);
  eot::mem::store<int16_t>(state + 8, pad.thumb_lx);
  eot::mem::store<int16_t>(state + 10, pad.thumb_ly);
  eot::mem::store<int16_t>(state + 12, pad.thumb_rx);
  eot::mem::store<int16_t>(state + 14, pad.thumb_ry);
}

#define EOT_KEYS "Input/Keybinds/Edge of Time"

REXCVAR_DEFINE_STRING(eot_key_jump, "Space", EOT_KEYS, "Jump key (A)");
REXCVAR_DEFINE_STRING(eot_key_light_attack, "LMB", EOT_KEYS, "Light attack key (X)");
REXCVAR_DEFINE_STRING(eot_key_heavy_attack, "MMB", EOT_KEYS, "Heavy attack key (Y)");
REXCVAR_DEFINE_STRING(eot_key_web, "E", EOT_KEYS, "Web / interact key (B)");
REXCVAR_DEFINE_STRING(eot_key_grab, "Q", EOT_KEYS, "Grab key (RB)");
REXCVAR_DEFINE_STRING(eot_key_special_attack, "V", EOT_KEYS, "Throw key (LB)");
REXCVAR_DEFINE_STRING(eot_key_web_swing, "RMB", EOT_KEYS, "Web swing key (RT)");
REXCVAR_DEFINE_STRING(eot_key_hyper_sense, "Shift", EOT_KEYS, "Hyper-Sense key (LT)");
REXCVAR_DEFINE_STRING(eot_key_left_stick_click, "Z", EOT_KEYS, "Left stick click key");
REXCVAR_DEFINE_STRING(eot_key_right_stick_click, "X", EOT_KEYS, "Right stick click key");
REXCVAR_DEFINE_STRING(eot_key_time_paradox, "", EOT_KEYS, "Time Stop key");
REXCVAR_DEFINE_STRING(eot_key_spider_sense, "R", EOT_KEYS, "Spider-Sense key (D-pad up)");
REXCVAR_DEFINE_STRING(eot_key_upgrades, "Tab", EOT_KEYS, "Upgrades key (Back)");
REXCVAR_DEFINE_STRING(eot_key_pause, "Escape", EOT_KEYS, "Pause key (Start)");
REXCVAR_DEFINE_STRING(eot_key_dpad_down, "C", EOT_KEYS, "D-pad down key");
REXCVAR_DEFINE_STRING(eot_key_dpad_left, "", EOT_KEYS, "D-pad left key");
REXCVAR_DEFINE_STRING(eot_key_dpad_right, "", EOT_KEYS, "D-pad right key");

namespace eot::controller {

namespace {

constexpr uint16_t kMicrosoft = 0x045E;
constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;
constexpr uint16_t kValve = 0x28DE;
constexpr uint16_t kSteamDeck = 0x1205;

// The original Steam Controller, the HEADCRAB one and the new one with its dongles.
bool IsSteamController(uint16_t product) {
  switch (product) {
  case 0x1101:
  case 0x1102:
  case 0x1105:
  case 0x1106:
  case 0x1142:
  case 0x1201:
  case 0x1202:
  case 0x1302:
  case 0x1303:
  case 0x1304:
  case 0x1305:
    return true;
  default:
    return false;
  }
}

uint16_t GuidWord(std::string_view guid, size_t byte) {
  if (guid.size() < (byte + 2) * 2)
    return 0;
  const std::string lo(guid.substr(byte * 2, 2));
  const std::string hi(guid.substr(byte * 2 + 2, 2));
  return static_cast<uint16_t>(std::strtoul(lo.c_str(), nullptr, 16) |
                               (std::strtoul(hi.c_str(), nullptr, 16) << 8));
}

bool Has(std::string_view name, std::string_view word) { return name.find(word) != std::string_view::npos; }

PadBrand BrandOf(const rex::input::DeviceInfo &info) {
  if (info.synthetic)
    return PadBrand::Keyboard;
  const uint16_t vendor = GuidWord(info.guid, 4);
  const uint16_t product = GuidWord(info.guid, 8);
  const std::string_view name = info.name;
  if (vendor == kValve && product == kSteamDeck)
    return PadBrand::SteamDeck;
  // Steam Input hands a Steam Controller over under its own name.
  if ((vendor == kValve && IsSteamController(product)) || Has(name, "Steam Controller"))
    return PadBrand::SteamController;
  if (vendor == kValve)
    return PadBrand::Unknown;
  if (vendor == kSony || Has(name, "PS4") || Has(name, "PS5") || Has(name, "DualSense") || Has(name, "DualShock"))
    return PadBrand::PlayStation;
  if (vendor == kNintendo || Has(name, "Switch") || Has(name, "Joy-Con"))
    return PadBrand::Switch;
  if (Has(name, "Steam Deck"))
    return PadBrand::SteamDeck;
  if (vendor == kMicrosoft || Has(name, "Xbox"))
    return Has(name, "360") ? PadBrand::Xbox360 : PadBrand::XboxSeries;
  return PadBrand::Unknown;
}

}

const char *ToString(PadBrand brand) {
  switch (brand) {
  case PadBrand::Xbox360:
    return "xbox";
  case PadBrand::XboxSeries:
    return "xboxseries";
  case PadBrand::PlayStation:
    return "playstation";
  case PadBrand::Switch:
    return "switch";
  case PadBrand::SteamDeck:
    return "steamdeck";
  case PadBrand::SteamController:
    return "steamcontroller";
  case PadBrand::Keyboard:
    return "keyboard";
  default:
    return "unknown";
  }
}

namespace {

// The first player's pad, as the SDK's slot assignment hands it out.
class TrackedAssignment final : public rex::input::DeviceAssignment {
public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo> &devices) override {
    inner_.OnDevicesChanged(devices);
    std::vector<rex::input::DeviceId> ids;
    inner_.DevicesForUser(0, ids);
    const rex::input::DeviceInfo *pad = nullptr;
    for (const rex::input::DeviceId id : ids) {
      for (const rex::input::DeviceInfo &d : devices)
        if (d.id == id && !d.synthetic)
          pad = &d;
      if (pad)
        break;
    }
    g_has_pad.store(pad != nullptr, std::memory_order_release);
    if (!pad) {
      EOT_INFO("[pad] no controller for the first player");
      return;
    }
    const PadBrand brand = BrandOf(*pad);
    g_pad_brand.store(brand, std::memory_order_release);
    EOT_INFO("[pad] the first player's controller: {} ({})", pad->name, ToString(brand));
  }

  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId> &out) const override {
    inner_.DevicesForUser(user_index, out);
  }

private:
  rex::input::SlotAssignment inner_;
};

}

PadBrand ActivePad() {
  const bool keyboard = MnkMode();
  if (!g_has_pad.load(std::memory_order_acquire))
    return keyboard ? PadBrand::Keyboard : PadBrand::Unknown;
  if (keyboard && g_keys.LastUsed() > g_pad_used.load(std::memory_order_acquire))
    return PadBrand::Keyboard;
  return g_pad_brand.load(std::memory_order_acquire);
}

void AttachKeyboard(rex::ui::Window *window) { g_keys.Attach(window); }

std::unique_ptr<rex::input::DeviceAssignment> MakeTrackedAssignment() {
  return std::make_unique<TrackedAssignment>();
}

}
