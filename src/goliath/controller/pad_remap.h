// goliath/controller/pad_remap.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <memory>

#include "goliath/controller/pad_actions.h"

namespace rex::ui {
class Window;
}

namespace rex::input {
class DeviceAssignment;
}

namespace eot::controller {

PadInput PhysicalFor(const PadAction &action);
bool SticksSwapped();
void PadRemapChanged();
bool PadRemapActive();

struct RawPad {
  uint16_t buttons;
  uint8_t left_trigger, right_trigger;
  int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
void RemapPad(RawPad &pad);

}

namespace eot::controller {

enum class PadBrand {
  Unknown,
  Xbox360,
  XboxSeries,
  PlayStation,
  Switch,
  SteamDeck,
  SteamController,
  Keyboard,
};

const char *ToString(PadBrand brand);

PadBrand ActivePad();

void AttachKeyboard(rex::ui::Window *window);

std::unique_ptr<rex::input::DeviceAssignment> MakeTrackedAssignment();

}
