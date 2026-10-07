// goliath/controller/pad_actions.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <string_view>

namespace eot::controller {

enum class PadInput : uint8_t {
  None,
  A,
  B,
  X,
  Y,
  LB,
  RB,
  LT,
  RT,
  LS,
  RS,
  Back,
  Start,
  Up,
  Down,
  Left,
  Right,
  LSRS,
  Count
};

struct PadInputInfo {
  PadInput input;
  const char *name;
  uint16_t bit;
  uint8_t slot;
};
inline constexpr PadInputInfo kPadInputs[] = {
    {PadInput::None, "", 0, 0xFF},
    {PadInput::A, "A", 0x1000, 0x00},
    {PadInput::B, "B", 0x2000, 0x01},
    {PadInput::X, "X", 0x4000, 0x03},
    {PadInput::Y, "Y", 0x8000, 0x02},
    {PadInput::LB, "LB", 0x0100, 0x0B},
    {PadInput::RB, "RB", 0x0200, 0x0A},
    {PadInput::LT, "LT", 0, 0x05},
    {PadInput::RT, "RT", 0, 0x04},
    {PadInput::LS, "LS", 0x0040, 0x07},
    {PadInput::RS, "RS", 0x0080, 0x06},
    {PadInput::Back, "Back", 0x0020, 0x08},
    {PadInput::Start, "Start", 0x0010, 0x09},
    {PadInput::Up, "Up", 0x0001, 0x1E},
    {PadInput::Down, "Down", 0x0002, 0xFF},
    {PadInput::Left, "Left", 0x0004, 0xFF},
    {PadInput::Right, "Right", 0x0008, 0xFF},
    {PadInput::LSRS, "LS+RS", 0x00C0, 0xFF},
};
static_assert(sizeof(kPadInputs) / sizeof(kPadInputs[0]) == static_cast<size_t>(PadInput::Count));

inline constexpr const PadInputInfo &PadInputInfoOf(PadInput input) {
  const size_t i = static_cast<size_t>(input);
  return kPadInputs[i < static_cast<size_t>(PadInput::Count) ? i : 0];
}

inline constexpr const char *PadInputName(PadInput input) { return PadInputInfoOf(input).name; }
inline constexpr uint16_t PadInputBit(PadInput input) { return PadInputInfoOf(input).bit; }
inline constexpr uint8_t PadInputSlot(PadInput input) { return PadInputInfoOf(input).slot; }

inline PadInput ParsePadInput(std::string_view name) {
  while (!name.empty() && name.front() == ' ')
    name.remove_prefix(1);
  while (!name.empty() && name.back() == ' ')
    name.remove_suffix(1);
  for (const PadInputInfo &i : kPadInputs)
    if (i.input != PadInput::None && name == i.name)
      return i.input;
  if (name == "L3")
    return PadInput::LS;
  if (name == "R3")
    return PadInput::RS;
  if (name == "DpadUp")
    return PadInput::Up;
  if (name == "DpadDown")
    return PadInput::Down;
  if (name == "DpadLeft")
    return PadInput::Left;
  if (name == "DpadRight")
    return PadInput::Right;
  return PadInput::None;
}

struct PadAction {
  const char *id;
  const char *key_cvar;
  const char *pad_cvar;
  PadInput native;
};
inline constexpr PadAction kPadActions[] = {
    {"jump", "eot_key_jump", "eot_pad_jump", PadInput::A},
    {"web", "eot_key_web", "eot_pad_web", PadInput::B},
    {"light_attack", "eot_key_light_attack", "eot_pad_light_attack", PadInput::X},
    {"heavy_attack", "eot_key_heavy_attack", "eot_pad_heavy_attack", PadInput::Y},
    {"grab", "eot_key_grab", "eot_pad_grab", PadInput::RB},
    {"special_attack", "eot_key_special_attack", "eot_pad_special_attack", PadInput::LB},
    {"web_swing", "eot_key_web_swing", "eot_pad_web_swing", PadInput::RT},
    {"hyper_sense", "eot_key_hyper_sense", "eot_pad_hyper_sense", PadInput::LT},
    {"spider_sense", "eot_key_spider_sense", "eot_pad_spider_sense", PadInput::Up},
    {"left_stick_click", "eot_key_left_stick_click", "eot_pad_left_stick_click", PadInput::LS},
    {"right_stick_click", "eot_key_right_stick_click", "eot_pad_right_stick_click", PadInput::RS},
    {"pause", "eot_key_pause", "eot_pad_pause", PadInput::Start},
    {"upgrades", "eot_key_upgrades", "eot_pad_upgrades", PadInput::Back},
    {"time_paradox", "eot_key_time_paradox", "eot_pad_time_paradox", PadInput::LSRS},
};
inline constexpr uint32_t kPadActionCount = sizeof(kPadActions) / sizeof(kPadActions[0]);

inline constexpr const char *kPadSticksCvar = "eot_pad_sticks";
inline constexpr const char *kMoveKeyCvars[4] = {"keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left",
                                                 "keybind_lstick_right"};
inline constexpr const char *kLookKeyCvars[4] = {"keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left",
                                                 "keybind_rstick_right"};

inline constexpr uint8_t kLeftStickDirSlots[4] = {0x1A, 0x1B, 0x19, 0x1D};
inline constexpr uint8_t kRightStickDirSlots[4] = {0x17, 0x18, 0x1C, 0x16};
inline constexpr uint8_t kLeftStickPressedSlot = 0x11;
inline constexpr uint8_t kRightStickPressedSlot = 0x10;
inline constexpr uint8_t kMoveKeyCapSlots[4] = {0x14, 0x15, 0x16, 0x17};
inline constexpr uint8_t kLookKeyCapSlots[4] = {0x18, 0x19, 0x1A, 0x1C};
inline constexpr uint8_t kLeftClickCapSlot = 0x0C;
inline constexpr uint8_t kRightClickCapSlot = 0x0D;
inline constexpr uint8_t kTimeParadoxCapSlot = 0x0E;

}
