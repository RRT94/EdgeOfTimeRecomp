// goliath/controller/bind_capture.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#define EOT_BINDS_HOST
#include "goliath/controller/bind_capture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "core/logging.h"
#include "goliath/controller/binds_api.h"
#include "goliath/controller/button_glyphs.h"
#include "goliath/controller/mouse_input.h"

namespace eot::controller {

namespace {

constexpr size_t kZOrder = 26;
constexpr auto kTimeout = std::chrono::seconds(8);

using clock = std::chrono::steady_clock;

struct Capture {
  std::mutex mutex;
  int32_t kinds = 0;
  int32_t result = EOT_BINDS_IDLE;
  std::string name;
  clock::time_point armed_at;
  uint16_t prev_buttons = 0;
  bool prev_lt = false, prev_rt = false;
  bool prev_valid = false;
  PadInput holding = PadInput::None;
  bool ctrl_edge = false;
};
Capture g_capture;

constexpr const char *kDevOverlayBinds[] = {"bind_debug_overlay", "bind_settings", "bind_achievements"};

bool IsDevOverlayKey(rex::ui::VirtualKey vk) {
  for (const char *bind : kDevOverlayBinds)
    if (rex::ui::ParseVirtualKey(rex::cvar::GetFlagByName(bind)) == vk)
      return true;
  return false;
}

bool IsCtrl(rex::ui::VirtualKey vk) {
  return vk == rex::ui::VirtualKey::kControl || vk == rex::ui::VirtualKey::kLControl ||
         vk == rex::ui::VirtualKey::kRControl;
}

void Finish(Capture &c, int32_t result, std::string name) {
  c.kinds = 0;
  c.result = result;
  c.name = std::move(name);
  EOT_DEBUG("[binds] capture: {}{}{}",
            result == EOT_BINDS_GOT_KEY ? "key" : result == EOT_BINDS_GOT_PAD ? "pad" : result == EOT_BINDS_CANCELLED ? "cancelled" : result == EOT_BINDS_CLEARED ? "cleared" : "timed out",
            c.name.empty() ? "" : " ", c.name);
}

class KeyCapture final : public rex::ui::WindowInputListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || attached_)
      return;
    attached_ = true;
    window->AddInputListener(this, kZOrder);
  }

  void OnKeyDown(rex::ui::KeyEvent &e) override {
    std::lock_guard<std::mutex> lock(g_capture.mutex);
    const rex::ui::VirtualKey vk = e.virtual_key();
    if (!g_capture.kinds) {
      if (IsCtrl(vk) && !e.prev_state())
        g_capture.ctrl_edge = true;
      if (!DebugModeActive() && IsDevOverlayKey(vk))
        e.set_handled(true);
      return;
    }
    if (e.prev_state())
      return;
    if (vk == rex::ui::VirtualKey::kEscape) {
      Finish(g_capture, EOT_BINDS_CANCELLED, "");
      e.set_handled(true);
      return;
    }
    if (!(g_capture.kinds & EOT_BINDS_KEY))
      return;
    if (vk == rex::ui::VirtualKey::kBack) {
      Finish(g_capture, EOT_BINDS_CLEARED, "");
    } else {
      rex::ui::VirtualKey named = vk;
      if (vk == rex::ui::VirtualKey::kLShift || vk == rex::ui::VirtualKey::kRShift)
        named = rex::ui::VirtualKey::kShift;
      else if (IsCtrl(vk))
        named = rex::ui::VirtualKey::kControl;
      else if (vk == rex::ui::VirtualKey::kLMenu || vk == rex::ui::VirtualKey::kRMenu)
        named = rex::ui::VirtualKey::kMenu;
      const std::string name = rex::ui::VirtualKeyToString(named);
      if (name.empty())
        return;
      Finish(g_capture, EOT_BINDS_GOT_KEY, name);
    }
    e.set_handled(true);
  }

  void OnMouseDown(rex::ui::MouseEvent &e) override {
    std::lock_guard<std::mutex> lock(g_capture.mutex);
    if (!(g_capture.kinds & EOT_BINDS_KEY))
      return;
    const char *name = nullptr;
    switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      name = "LMB";
      break;
    case rex::ui::MouseEvent::Button::kRight:
      name = "RMB";
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      name = "MMB";
      break;
    default:
      return;
    }
    Finish(g_capture, EOT_BINDS_GOT_KEY, name);
    e.set_handled(true);
  }

private:
  bool attached_ = false;
};
KeyCapture g_keys;

PadInput NewlyPressed(const Capture &c, const RawPad &pad) {
  const bool lt = pad.left_trigger > 30, rt = pad.right_trigger > 30;
  if (lt && !c.prev_lt)
    return PadInput::LT;
  if (rt && !c.prev_rt)
    return PadInput::RT;
  const uint16_t edge = static_cast<uint16_t>(pad.buttons & ~c.prev_buttons);
  for (uint32_t i = 1; i < static_cast<uint32_t>(PadInput::Count); ++i) {
    const PadInput input = static_cast<PadInput>(i);
    const uint16_t bit = PadInputBit(input);
    if (bit && (edge & bit))
      return input;
  }
  return PadInput::None;
}

bool StillDown(const RawPad &pad, PadInput input) {
  switch (input) {
  case PadInput::LT:
    return pad.left_trigger > 30;
  case PadInput::RT:
    return pad.right_trigger > 30;
  case PadInput::None:
    return false;
  default:
    return (pad.buttons & PadInputBit(input)) != 0;
  }
}

void Neutralise(RawPad &pad) {
  pad.buttons = 0;
  pad.left_trigger = pad.right_trigger = 0;
  pad.thumb_lx = pad.thumb_ly = pad.thumb_rx = pad.thumb_ry = 0;
}

}

void AttachBindCapture(rex::ui::Window *window) { g_keys.Attach(window); }

bool FilterPadForCapture(RawPad &pad) {
  std::lock_guard<std::mutex> lock(g_capture.mutex);
  Capture &c = g_capture;
  const bool armed = (c.kinds & EOT_BINDS_PAD) != 0;
  if (c.holding != PadInput::None) {
    if (StillDown(pad, c.holding)) {
      c.prev_buttons = pad.buttons;
      c.prev_lt = pad.left_trigger > 30;
      c.prev_rt = pad.right_trigger > 30;
      Neutralise(pad);
      return true;
    }
    c.holding = PadInput::None;
  }
  if (!armed) {
    c.prev_valid = false;
    return false;
  }
  if (c.prev_valid) {
    const PadInput input = NewlyPressed(c, pad);
    if (input != PadInput::None) {
      c.holding = input;
      Finish(c, EOT_BINDS_GOT_PAD, PadInputName(input));
    } else if (clock::now() - c.armed_at > kTimeout) {
      Finish(c, EOT_BINDS_TIMED_OUT, "");
    }
  }
  c.prev_buttons = pad.buttons;
  c.prev_lt = pad.left_trigger > 30;
  c.prev_rt = pad.right_trigger > 30;
  c.prev_valid = true;
  Neutralise(pad);
  return true;
}

}

using namespace eot::controller;

int32_t eot_binds_capture_begin(int32_t kinds) {
  std::lock_guard<std::mutex> lock(g_capture.mutex);
  if (!(kinds & (EOT_BINDS_KEY | EOT_BINDS_PAD)))
    return 0;
  g_capture.kinds = kinds;
  g_capture.result = EOT_BINDS_WAITING;
  g_capture.name.clear();
  g_capture.armed_at = std::chrono::steady_clock::now();
  g_capture.prev_valid = false;
  g_capture.ctrl_edge = false;
  EOT_DEBUG("[binds] capture armed for {}{}", (kinds & EOT_BINDS_KEY) ? "a key" : "",
            (kinds & EOT_BINDS_PAD) ? ((kinds & EOT_BINDS_KEY) ? " or a pad button" : "a pad button") : "");
  return 1;
}

int32_t eot_binds_capture_poll(char *name, int32_t size) {
  std::lock_guard<std::mutex> lock(g_capture.mutex);
  Capture &c = g_capture;
  if (c.kinds && c.result == EOT_BINDS_WAITING && (c.kinds & EOT_BINDS_KEY) &&
      std::chrono::steady_clock::now() - c.armed_at > std::chrono::seconds(8))
    Finish(c, EOT_BINDS_TIMED_OUT, "");
  const int32_t result = c.result;
  if (name && size > 0) {
    std::strncpy(name, c.name.c_str(), static_cast<size_t>(size) - 1);
    name[size - 1] = 0;
  }
  if (result != EOT_BINDS_WAITING && result != EOT_BINDS_IDLE) {
    c.result = EOT_BINDS_IDLE;
    c.name.clear();
  }
  return result;
}

void eot_binds_capture_end(void) {
  std::lock_guard<std::mutex> lock(g_capture.mutex);
  g_capture.kinds = 0;
  g_capture.result = EOT_BINDS_IDLE;
  g_capture.name.clear();
}

int32_t eot_binds_ctrl_pressed(void) {
  std::lock_guard<std::mutex> lock(g_capture.mutex);
  const bool edge = g_capture.ctrl_edge;
  g_capture.ctrl_edge = false;
  return edge ? 1 : 0;
}

void eot_binds_changed(void) {
  PadRemapChanged();
  GlyphBindsChanged();
}

void eot_prompts_bar_object(int32_t object) { NoteButtonHelper(static_cast<uint32_t>(object)); }

void eot_prompts_bar_zone(int32_t zone) { NoteButtonHelperZone(static_cast<uint32_t>(zone)); }

void eot_kbm_mash_prompt(int32_t buttons) { NoteMashPrompt(static_cast<uint16_t>(buttons)); }

void eot_kbm_finish_prompt(int32_t on) { NoteFinishPrompt(on != 0); }

int32_t eot_binds_key_glyph(int32_t slot) { return slot >= 0 && slot < 32 && KeyCapInstalled(static_cast<uint8_t>(slot)) ? 1 : 0; }

int32_t eot_binds_pad_glyph(int32_t slot) {
  return slot >= 0 && slot < 64 && MenuGlyphInstalled(static_cast<uint8_t>(slot)) ? 1 : 0;
}

namespace {
std::atomic<bool> g_debug_mode{false};
}

void eot_debug_mode_active(int32_t on) { g_debug_mode.store(on != 0, std::memory_order_release); }

namespace eot::controller {
bool DebugModeActive() { return g_debug_mode.load(std::memory_order_acquire); }
}
