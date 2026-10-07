// goliath/controller/mouse_input.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/controller/mouse_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/window_listener.h>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <rex/kernel/xam/module.h>
#include <rex/ui/virtual_key.h>

#include "core/logging.h"
#include "gpu/settings.h"
#include "core/memory_helpers.h"
#include "goliath/controller/button_glyphs.h"
#include "goliath/controller/pad_remap.h"

namespace eot::controller {

namespace {

constexpr size_t kZOrder = 24;

using clock = std::chrono::steady_clock;
constexpr auto kMenuBarFresh = std::chrono::milliseconds(150);
constexpr float kMeantMotionPixels = 32.0f;
constexpr auto kMotionWindow = std::chrono::milliseconds(500);
constexpr auto kCursorRest = std::chrono::seconds(3);

std::atomic<int64_t> g_menu_bar_ns{0};

class MouseInput final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || window_)
      return;
    window_ = window;
    window->AddInputListener(this, kZOrder);
    window->AddListener(this);
  }

  void Take(float *dx, float *dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool grab = grab_.load(std::memory_order_relaxed);
    const bool live = focused_ && !grab && fullscreen_.load(std::memory_order_relaxed) &&
                      !cursor_shown_;
    const float turn = (focused_ && !grab) ? turn_ : 0.0f;
    if (dx)
      *dx = (live ? dx_ : 0.0f) + turn;
    if (dy)
      *dy = live ? dy_ : 0.0f;
    turn_ = 0.0f;
    if (!grab)
      dx_ = dy_ = 0.0f;
  }

  void AddTurn(float counts) {
    if (grab_.load(std::memory_order_relaxed))
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    turn_ += counts;
  }

  void TakeForDebug(float *dx, float *dy, int *wheel) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool live = focused_ && grab_.load(std::memory_order_relaxed) && !cursor_shown_;
    if (dx)
      *dx = live ? dx_ : 0.0f;
    if (dy)
      *dy = live ? dy_ : 0.0f;
    if (wheel)
      *wheel = live ? wheel_ : 0;
    dx_ = dy_ = 0.0f;
    wheel_ = 0;
  }

  void SetGrab(bool on) {
    if (grab_.exchange(on, std::memory_order_acq_rel) == on)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dx_ = dy_ = 0.0f;
      wheel_ = 0;
      cursor_shown_ = false;
    }
    rex::input::mnk::SetMouseLookActive(on || fullscreen_.load(std::memory_order_relaxed));
    if (window_)
      window_->SetCursorVisibility(on || fullscreen_.load(std::memory_order_relaxed)
                                       ? rex::ui::Window::CursorVisibility::kHidden
                                       : rex::ui::Window::CursorVisibility::kVisible);
    EOT_INFO("[input] the free camera {} the mouse", on ? "takes" : "hands back");
  }

  void ApplyPolicy(bool fullscreen) {
    fullscreen_.store(fullscreen, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cursor_shown_ = false;
      meant_motion_ = 0.0f;
    }
    rex::input::mnk::SetMouseLookActive(fullscreen);
    if (window_)
      window_->SetCursorVisibility(fullscreen ? rex::ui::Window::CursorVisibility::kHidden
                                              : rex::ui::Window::CursorVisibility::kVisible);
    EOT_DEBUG("[input] {}: mouse look {}, cursor {}", fullscreen ? "fullscreen" : "windowed",
              fullscreen ? "on" : "off", fullscreen ? "hidden" : "visible");
  }

  void Tick(bool overlay_wants_pointer) {
    const bool grab = grab_.load(std::memory_order_relaxed);
    if (!window_ || (!grab && !fullscreen_.load(std::memory_order_relaxed)))
      return;
    if (grab) {
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = overlay_wants_pointer != cursor_shown_;
        cursor_shown_ = overlay_wants_pointer;
      }
      if (changed)
        rex::input::mnk::SetMouseLookActive(!overlay_wants_pointer);
      const auto want = overlay_wants_pointer ? rex::ui::Window::CursorVisibility::kVisible
                                              : rex::ui::Window::CursorVisibility::kHidden;
      if (changed || window_->GetCursorVisibility() != want)
        window_->SetCursorVisibility(want);
      return;
    }
    const auto now = clock::now();
    const bool menu = now.time_since_epoch().count() - g_menu_bar_ns.load(std::memory_order_acquire) <=
                      std::chrono::duration_cast<clock::duration>(kMenuBarFresh).count();
    bool want = overlay_wants_pointer;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (menu && focused_) {
        if (cursor_shown_ ? now - last_motion_ <= kCursorRest : meant_motion_ >= kMeantMotionPixels)
          want = true;
      }
      if (!menu)
        meant_motion_ = 0.0f;
      changed = want != cursor_shown_;
      cursor_shown_ = want;
      if (changed)
        meant_motion_ = 0.0f;
    }
    if (changed) {
      rex::input::mnk::SetMouseLookActive(!want);
      EOT_DEBUG("[input] cursor {}: {}", want ? "shown" : "hidden",
                want ? (overlay_wants_pointer ? "an overlay takes input" : "the mouse moved on a menu")
                    : (menu ? "the mouse rested" : "no menu, the camera has the mouse"));
    }
    const auto visibility =
        want ? rex::ui::Window::CursorVisibility::kVisible : rex::ui::Window::CursorVisibility::kHidden;
    if (changed || (want && window_->GetCursorVisibility() != visibility))
      window_->SetCursorVisibility(visibility);
  }

  void OnMouseWheel(rex::ui::MouseEvent &e) override {
    if (!grab_.load(std::memory_order_relaxed))
      return;
    const int notches = e.scroll_y() / static_cast<int>(rex::ui::MouseEvent::kScrollPerDetent);
    if (!notches)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    wheel_ = std::clamp(wheel_ + notches, -8, 8);
  }

  void OnMouseMove(rex::ui::MouseEvent &e) override {
    std::lock_guard<std::mutex> lock(mutex_);
    float mx = 0.0f, my = 0.0f;
    if (e.dx() != 0.0f || e.dy() != 0.0f) {
      mx = e.dx();
      my = e.dy();
    } else if (have_position_) {
      mx = static_cast<float>(e.x() - x_);
      my = static_cast<float>(e.y() - y_);
    }
    dx_ += mx;
    dy_ += my;
    x_ = e.x();
    y_ = e.y();
    have_position_ = true;
    if (mx != 0.0f || my != 0.0f) {
      const auto now = clock::now();
      if (now - last_motion_ > kMotionWindow)
        meant_motion_ = 0.0f;
      meant_motion_ += std::fabs(mx) + std::fabs(my);
      last_motion_ = now;
    }
  }

  void OnLostFocus(rex::ui::UISetupEvent &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = false;
    dx_ = dy_ = 0.0f;
    meant_motion_ = 0.0f;
    have_position_ = false;
  }
  void OnGotFocus(rex::ui::UISetupEvent &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = true;
    have_position_ = false;
  }
  void OnClosing(rex::ui::UIEvent &) override {
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
      window_ = nullptr;
    }
  }

private:
  rex::ui::Window *window_ = nullptr;
  std::atomic<bool> fullscreen_{true};
  std::mutex mutex_;
  float dx_ = 0.0f, dy_ = 0.0f;
  int32_t x_ = 0, y_ = 0;
  bool have_position_ = false;
  bool focused_ = true;
  bool cursor_shown_ = false;
  float turn_ = 0.0f;
  int wheel_ = 0;
  std::atomic<bool> grab_{false};
  float meant_motion_ = 0.0f;
  clock::time_point last_motion_{};
};

MouseInput g_mouse;

}

void AttachMouseInput(rex::ui::Window *window) {
  g_mouse.Attach(window);
  g_mouse.ApplyPolicy(eot::gpu::Settings::Fullscreen());
  rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
    const bool fullscreen = value == "true" || value == "1";
    rex::Runtime *runtime = rex::Runtime::instance();
    if (runtime && runtime->app_context())
      runtime->app_context()->CallInUIThread([fullscreen] { g_mouse.ApplyPolicy(fullscreen); });
    else
      g_mouse.ApplyPolicy(fullscreen);
  });
}

void MouseCursorTick(bool overlay_wants_pointer) { g_mouse.Tick(overlay_wants_pointer); }

void MouseGrabForDebug(bool on) { g_mouse.SetGrab(on); }

void MouseTakeForDebug(float *dx, float *dy, int *wheel) { g_mouse.TakeForDebug(dx, dy, wheel); }

void MouseAddTurn(float counts) { g_mouse.AddTurn(counts); }

void NoteMenuBarShown() {
  g_menu_bar_ns.store(clock::now().time_since_epoch().count(), std::memory_order_release);
}

}

extern "C" EOT_EXPORT void eot_mouse_take(float *dx, float *dy) { eot::controller::g_mouse.Take(dx, dy); }

namespace eot::controller {

namespace {

constexpr size_t kMenuZOrder = 25;

REXCVAR_DEFINE_DOUBLE(eot_wheel_camera_turn, 60.0, "EdgeOfTime/Input", "Scroll wheel camera turn");

constexpr auto kMashFresh = std::chrono::milliseconds(200);

constexpr uint32_t kActionSlots = 0x883C92F8;
constexpr uint32_t kActionSlotBusy = 0x883C930C;
constexpr uint32_t kActionSlotCount = 5;

constexpr uint32_t kPressHold = 1;
constexpr uint32_t kPressGap = 2;
constexpr uint32_t kWheelHold = 3;
constexpr int32_t kWheelMax = 4;

std::atomic<bool> g_escape{false};
std::atomic<bool> g_delete{false};
std::atomic<bool> g_backspace{false};
std::atomic<bool> g_space{false};
std::atomic<bool> g_left_button{false};
std::atomic<uint32_t> g_press_a{0};
std::atomic<uint32_t> g_press_b{0};
std::atomic<uint32_t> g_press_x{0};
std::atomic<int32_t> g_wheel{0};
std::atomic<int64_t> g_mash_ns{0};
std::atomic<uint16_t> g_mash_buttons{0};
std::atomic<int64_t> g_finish_ns{0};

struct Pulse {
  uint32_t taken = 0;
  uint32_t left = 0;
  uint32_t gap = 0;

  bool Step(std::atomic<uint32_t> &presses) {
    if (left) {
      if (--left == 0)
        gap = kPressGap;
      return true;
    }
    if (gap) {
      --gap;
      return false;
    }
    if (presses.load(std::memory_order_acquire) == taken)
      return false;
    ++taken;
    left = kPressHold - 1;
    return true;
  }

  void Drop(const std::atomic<uint32_t> &presses) {
    taken = presses.load(std::memory_order_acquire);
    left = gap = 0;
  }
};
Pulse g_pulse_a, g_pulse_b, g_pulse_x;

std::atomic<uint16_t> g_from_left_button{0};
std::atomic<uint16_t> g_from_escape{0};
std::atomic<uint16_t> g_from_delete{0};
std::atomic<uint16_t> g_from_space{0};
clock::time_point g_keys_read{};
constexpr auto kKeysInterval = std::chrono::milliseconds(500);

std::string FirstKey(const char *cvar) {
  std::string value = rex::cvar::GetFlagByName(cvar);
  if (const size_t comma = value.find(','); comma != std::string::npos)
    value.resize(comma);
  if (const size_t plus = value.rfind('+'); plus != std::string::npos)
    value.erase(0, plus + 1);
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(0, 1);
  return value;
}

uint16_t ButtonsFor(std::string_view key) {
  uint16_t buttons = 0;
  for (const PadAction &action : kPadActions)
    if (FirstKey(action.key_cvar) == key)
      buttons |= PadInputBit(PhysicalFor(action));
  return buttons;
}

void RefreshKeyButtons() {
  const clock::time_point now = clock::now();
  if (g_keys_read != clock::time_point{} && now - g_keys_read < kKeysInterval)
    return;
  g_keys_read = now;
  g_from_left_button.store(ButtonsFor("LMB"), std::memory_order_relaxed);
  g_from_escape.store(ButtonsFor("Escape"), std::memory_order_relaxed);
  g_from_delete.store(static_cast<uint16_t>(ButtonsFor("Delete") | ButtonsFor("Backspace")),
                      std::memory_order_relaxed);
  g_from_space.store(ButtonsFor("Space"), std::memory_order_relaxed);
}

bool KeyboardInHand() { return ActivePad() == PadBrand::Keyboard; }

bool InMenu() { return KeyboardInHand() && BarShowsPrompts(); }

bool WheelInMenu() { return BarShowsPrompts(); }

bool ContextualActionOnOffer() {
  static bool mapped = false;
  if (!mapped) {
    if (!eot::mem::readable(kActionSlots, 4 * kActionSlotCount) ||
        !eot::mem::readable(kActionSlotBusy, kActionSlotCount))
      return false;
    mapped = true;
  }
  for (uint32_t slot = 0; slot < kActionSlotCount; ++slot)
    if (eot::mem::load<uint32_t>(kActionSlots + 4 * slot) != 0xFFFFFFFFu)
      return eot::mem::load<uint8_t>(kActionSlotBusy + slot) == 0;
  return false;
}

bool g_space_down = false;
bool g_space_acts = false;

bool MashFresh() {
  const int64_t at = g_mash_ns.load(std::memory_order_acquire);
  return at != 0 && clock::now().time_since_epoch().count() - at <=
                        std::chrono::duration_cast<clock::duration>(kMashFresh).count();
}

bool FinishOnOffer() {
  const int64_t at = g_finish_ns.load(std::memory_order_acquire);
  return at != 0 && clock::now().time_since_epoch().count() - at <=
                        std::chrono::duration_cast<clock::duration>(kMashFresh).count();
}

class MenuKeys final : public rex::ui::WindowInputListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || attached_)
      return;
    attached_ = true;
    window->AddInputListener(this, kMenuZOrder);
  }

  void OnKeyDown(rex::ui::KeyEvent &e) override { Note(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Note(e.virtual_key(), false); }

  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (e.button() != rex::ui::MouseEvent::Button::kLeft)
      return;
    g_left_button.store(true, std::memory_order_release);
    g_press_a.fetch_add(1, std::memory_order_acq_rel);
  }
  void OnMouseUp(rex::ui::MouseEvent &e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kLeft)
      g_left_button.store(false, std::memory_order_release);
  }

  void OnMouseWheel(rex::ui::MouseEvent &e) override {
    const int32_t notches = e.scroll_y() / static_cast<int32_t>(rex::ui::MouseEvent::kScrollPerDetent);
    if (!notches)
      return;
    int32_t waiting = g_wheel.load(std::memory_order_relaxed);
    int32_t wanted = (waiting != 0 && (waiting > 0) != (notches > 0)) ? notches : waiting + notches;
    wanted = wanted > kWheelMax ? kWheelMax : (wanted < -kWheelMax ? -kWheelMax : wanted);
    g_wheel.store(wanted, std::memory_order_release);
  }

private:
  static void Note(rex::ui::VirtualKey vk, bool down) {
    switch (vk) {
    case rex::ui::VirtualKey::kEscape:
      if (down && !g_escape.exchange(true, std::memory_order_acq_rel))
        g_press_b.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_escape.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kDelete:
      if (down && !g_delete.exchange(true, std::memory_order_acq_rel))
        g_press_x.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_delete.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kBack:
      if (down && !g_backspace.exchange(true, std::memory_order_acq_rel))
        g_press_x.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_backspace.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kSpace:
      g_space.store(down, std::memory_order_release);
      break;
    default:
      break;
    }
  }

  bool attached_ = false;
};
MenuKeys g_listener;

int32_t g_pulse_dir = 0;
uint32_t g_pulse_left = 0;
uint32_t g_gap_left = 0;

}

void AttachMenuKeys(rex::ui::Window *window) { g_listener.Attach(window); }

bool MenuKeysActive() { return InMenu(); }

void NoteFinishPrompt(bool on) {
  const int64_t was = g_finish_ns.exchange(on ? clock::now().time_since_epoch().count() : 0,
                                           std::memory_order_acq_rel);
  if (on && !was) {
    static bool told = false;
    if (!told) {
      told = true;
      EOT_DEBUG("[input] a finisher is in reach: the space bar answers it as B");
    }
  }
}

void NoteMashPrompt(uint16_t buttons) {
  const uint16_t was = g_mash_buttons.exchange(buttons, std::memory_order_acq_rel);
  g_mash_ns.store(clock::now().time_since_epoch().count(), std::memory_order_release);
  if (buttons != was)
    EOT_DEBUG("[input] a mash QTE counts pad buttons {:#06x}: the space bar presses them too", buttons);
}

void ApplyWheel(RawPad &pad) {
  if (!WheelInMenu()) {
    const int32_t waiting = g_wheel.exchange(0, std::memory_order_acq_rel);
    const double counts = waiting ? REXCVAR_GET(eot_wheel_camera_turn) : 0.0;
    if (counts > 0.0) {
      const double sens = std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str());
      MouseAddTurn(static_cast<float>(-waiting * counts / (sens > 0.05 ? sens : 1.0)));
    }
    g_pulse_dir = 0;
    g_pulse_left = g_gap_left = 0;
    return;
  }

  if (g_pulse_left) {
    --g_pulse_left;
    pad.buttons |= PadInputBit(g_pulse_dir > 0 ? PadInput::Right : PadInput::Left);
    if (!g_pulse_left)
      g_gap_left = kPressGap;
    return;
  }
  if (g_gap_left) {
    --g_gap_left;
    return;
  }
  const int32_t waiting = g_wheel.load(std::memory_order_acquire);
  if (!waiting)
    return;
  g_pulse_dir = waiting > 0 ? -1 : 1;
  g_wheel.store(waiting > 0 ? waiting - 1 : waiting + 1, std::memory_order_release);
  g_pulse_left = kWheelHold - 1;
  pad.buttons |= PadInputBit(g_pulse_dir > 0 ? PadInput::Right : PadInput::Left);
}

void ApplyMenuKeys(RawPad &pad) {
  if (rex::kernel::xam::xeXamIsUIActive()) {
    g_pulse_a.Drop(g_press_a);
    g_pulse_b.Drop(g_press_b);
    g_pulse_x.Drop(g_press_x);
    g_wheel.store(0, std::memory_order_release);
    g_pulse_dir = 0;
    g_pulse_left = g_gap_left = 0;
    g_space_acts = false;
    g_space_down = g_space.load(std::memory_order_acquire);
    return;
  }

  if (KeyboardInHand() && MashFresh() && g_space.load(std::memory_order_acquire))
    pad.buttons |= g_mash_buttons.load(std::memory_order_acquire);

  const bool space = KeyboardInHand() && g_space.load(std::memory_order_acquire);
  if (space && !g_space_down) {
    g_space_acts = !InMenu() && (ContextualActionOnOffer() || FinishOnOffer());
    if (g_space_acts) {
      RefreshKeyButtons();
      static bool told = false;
      if (!told) {
        told = true;
        EOT_DEBUG("[input] a contextual action is on offer: the space bar answers it as B");
      }
    }
  }
  if (!space)
    g_space_acts = false;
  g_space_down = space;
  if (g_space_acts) {
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_space.load(std::memory_order_relaxed));
    pad.buttons |= PadInputBit(PadInput::B);
  }

  ApplyWheel(pad);

  if (!InMenu()) {
    g_pulse_a.Drop(g_press_a);
    g_pulse_b.Drop(g_press_b);
    g_pulse_x.Drop(g_press_x);
    return;
  }
  RefreshKeyButtons();

  if (g_left_button.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_left_button.load(std::memory_order_relaxed));
  if (g_escape.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_escape.load(std::memory_order_relaxed));
  if (g_delete.load(std::memory_order_acquire) || g_backspace.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_delete.load(std::memory_order_relaxed));
  if (g_pulse_a.Step(g_press_a))
    pad.buttons |= PadInputBit(PadInput::A);
  if (g_pulse_b.Step(g_press_b))
    pad.buttons |= PadInputBit(PadInput::B);
  if (g_pulse_x.Step(g_press_x))
    pad.buttons |= PadInputBit(PadInput::X);
}

}
