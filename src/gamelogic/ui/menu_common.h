// gamelogic/ui/menu_common.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "core/quit_client.h"
#include "gamelogic/ui/hud_api.h"
#include "goliath/ui/menu_handles.h"
#include "core/export.h"
#include "goliath/controller/binds_api.h"
#include "mods/mods_api.h"

REX_EXTERN(__imp__eot_YesNoWindow_InitConfig);
REX_EXTERN(__imp__eot_YesNoWindow_Open);

namespace eot::ui {

inline constexpr uint32_t kDescCount = 0;
inline constexpr uint32_t kDescSelected = 4;
inline constexpr uint32_t kDescHandle0 = 12;
inline constexpr uint32_t kDescProp0 = 52;
inline constexpr uint32_t kDescSelectableMask = 92;
inline constexpr uint32_t kDescDisabledMask = 94;
inline constexpr uint32_t kDescMaxSlots = 10;
inline constexpr uint32_t kPropDefault = 16;

inline constexpr uint32_t kEvtType = 0;
inline constexpr uint32_t kEvtConsumed = 8;
inline constexpr uint32_t kEvtNav = 7;
inline constexpr uint32_t kEvtSelect = 9;
inline constexpr uint32_t kEvtBack = 10;
inline constexpr uint32_t kEvtStart = 12;

inline constexpr uint32_t kYesNoResultMessage = 0x0649D236;
inline constexpr uint32_t kResultHandleOff = 0;
inline constexpr uint32_t kResultValueOff = 8;
inline constexpr uint32_t kResultYes = 1;

struct YesNoLayout {
  uint32_t config;
  uint32_t titleIndex;
  uint32_t body;
  uint32_t handle;
  uint32_t state;
};

inline uint32_t DescHandleAddr(uint32_t desc, uint32_t i) { return desc + kDescHandle0 + i * 4; }
inline uint32_t DescPropAddr(uint32_t desc, uint32_t i) { return desc + kDescProp0 + i * 4; }

inline int AppendEntry(uint32_t desc, uint32_t handle) {
  const uint32_t count = eot::mem::load<uint32_t>(desc + kDescCount);
  if (count == 0 || count >= kDescMaxSlots)
    return -1;
  eot::mem::store<uint32_t>(DescHandleAddr(desc, count), handle);
  eot::mem::store<uint32_t>(DescPropAddr(desc, count), kPropDefault);
  eot::mem::store<uint32_t>(desc + kDescCount, count + 1);
  return static_cast<int>(count);
}

inline void ConsumeEvent(uint32_t event) {
  if (event)
    eot::mem::store<uint8_t>(event + kEvtConsumed, 0);
}

inline constexpr uint32_t kSoundApiPtr = 0x883CA204;
inline constexpr uint32_t kSoundCommitPtr = 0x883CA1DC;
inline constexpr uint32_t kSoundPlaySlot = 20;
inline constexpr uint32_t kSoundBank = 12;
inline constexpr uint32_t kCueAccept = 0x226397BB;
inline constexpr uint32_t kCueBack = 0xDDD5DB45;
inline constexpr uint32_t kCueUp = 0x1B325446;
inline constexpr uint32_t kCueDown = 0x46DE054A;
inline constexpr uint32_t kCueChange = 0xF4D03DBA;
inline constexpr uint32_t kCueDenied = 0x6C688CC5;

inline void PlayCue(const PPCContext &ctx, uint8_t *base, uint32_t cue) {
  const uint32_t api = eot::mem::load<uint32_t>(kSoundApiPtr);
  const uint32_t commit = eot::mem::load<uint32_t>(kSoundCommitPtr);
  if (!api || !commit)
    return;
  const uint32_t sound = hud::CallAt(ctx, base, eot::mem::load<uint32_t>(api + kSoundPlaySlot), kSoundBank, cue);
  hud::CallAt(ctx, base, eot::mem::load<uint32_t>(commit), sound);
}

inline constexpr uint32_t kMMMemoryMgrAlloc = 0x820820A8;
inline uint32_t AllocGuest(const PPCContext &ctx, uint8_t *base, uint32_t size) {
  const uint32_t block = hud::CallAt(ctx, base, kMMMemoryMgrAlloc, size, 16, 0xFFFFFFFFu, 0);
  return block == hud::kNoWindow ? 0 : block;
}

inline constexpr uint32_t kYesNoCfgWindow = 56;
inline constexpr uint32_t kYesNoCfgYes = 92;
inline constexpr uint32_t kYesNoCfgNo = 96;

inline uint32_t OpenConfirm(const PPCContext &ctx, uint8_t *base, uint32_t self, const YesNoLayout &L,
                            uint32_t title, uint32_t body, uint32_t windowCrc = 0, uint32_t yesCrc = 0,
                            uint32_t noCrc = 0) {
  const uint32_t config = self + L.config;
  PPCContext call = ctx;
  call.r3.u32 = config;
  __imp__eot_YesNoWindow_InitConfig(call, base);
  const uint32_t title_index = eot::mem::load<uint32_t>(self + L.titleIndex);
  eot::mem::store<uint32_t>(config + (title_index + 8) * 4, title);
  eot::mem::store<uint32_t>(self + L.body, body);
  if (windowCrc) {
    eot::mem::store<uint32_t>(config + kYesNoCfgWindow, hud::Find(ctx, base, windowCrc));
    eot::mem::store<uint32_t>(config + kYesNoCfgYes, hud::Find(ctx, base, yesCrc));
    eot::mem::store<uint32_t>(config + kYesNoCfgNo, hud::Find(ctx, base, noCrc));
  }
  if (L.state)
    eot::mem::store<uint32_t>(self + L.state, 2);
  call = ctx;
  call.r3.u32 = config;
  __imp__eot_YesNoWindow_Open(call, base);
  const uint32_t handle = call.r3.u32;
  eot::mem::store<uint32_t>(self + L.handle, handle);
  return handle;
}

inline uint32_t OpenExitConfirm(const PPCContext &ctx, uint8_t *base, uint32_t self, const YesNoLayout &L) {
  return OpenConfirm(ctx, base, self, L, kHandleExitTitle, kHandleExitBody);
}

inline bool ExitIfConfirmed(uint32_t self, uint32_t message, uint32_t result,
                            const YesNoLayout &L) {
  if (message != kYesNoResultMessage || !self || !result)
    return false;
  const uint32_t handle = eot::mem::load<uint32_t>(result + kResultHandleOff);
  if (handle != eot::mem::load<uint32_t>(self + L.handle))
    return false;
  if (eot::mem::load<uint32_t>(result + kResultValueOff) == kResultYes)
    QuitProcessFromModule(0);
  return true;
}

}

namespace eot::ui {

constexpr uint32_t kPromptA = 1;
constexpr uint32_t kPromptBack = 15;
constexpr uint32_t kPromptY = 20;
constexpr uint32_t kPromptX = 27;

void OverridePrompt(uint32_t id, const char *name);
void RestorePrompt(uint32_t id);

void SendPromptMask(const PPCContext &ctx, uint8_t *base, uint32_t zone, uint32_t mask, uint32_t scratch);

}

namespace eot::ui::binds {

struct Host {
  int32_t (*capture_begin)(int32_t) = nullptr;
  int32_t (*capture_poll)(char *, int32_t) = nullptr;
  void (*capture_end)() = nullptr;
  int32_t (*ctrl_pressed)() = nullptr;
  void (*changed)() = nullptr;
  int32_t (*key_glyph)(int32_t) = nullptr;
  int32_t (*pad_glyph)(int32_t) = nullptr;
  void (*bar_object)(int32_t) = nullptr;
  void (*bar_zone)(int32_t) = nullptr;
  void (*mash_prompt)(int32_t) = nullptr;
  void (*finish_prompt)(int32_t) = nullptr;

  bool Bound() const { return capture_begin && capture_poll && capture_end && changed; }
};

inline const Host &Api() {
  static const Host host = [] {
    Host h;
    h.capture_begin = reinterpret_cast<decltype(h.capture_begin)>(HostEntryPoint("eot_binds_capture_begin"));
    h.capture_poll = reinterpret_cast<decltype(h.capture_poll)>(HostEntryPoint("eot_binds_capture_poll"));
    h.capture_end = reinterpret_cast<decltype(h.capture_end)>(HostEntryPoint("eot_binds_capture_end"));
    h.ctrl_pressed = reinterpret_cast<decltype(h.ctrl_pressed)>(HostEntryPoint("eot_binds_ctrl_pressed"));
    h.changed = reinterpret_cast<decltype(h.changed)>(HostEntryPoint("eot_binds_changed"));
    h.key_glyph = reinterpret_cast<decltype(h.key_glyph)>(HostEntryPoint("eot_binds_key_glyph"));
    h.pad_glyph = reinterpret_cast<decltype(h.pad_glyph)>(HostEntryPoint("eot_binds_pad_glyph"));
    h.bar_object = reinterpret_cast<decltype(h.bar_object)>(HostEntryPoint("eot_prompts_bar_object"));
    h.bar_zone = reinterpret_cast<decltype(h.bar_zone)>(HostEntryPoint("eot_prompts_bar_zone"));
    h.mash_prompt = reinterpret_cast<decltype(h.mash_prompt)>(HostEntryPoint("eot_kbm_mash_prompt"));
    h.finish_prompt = reinterpret_cast<decltype(h.finish_prompt)>(HostEntryPoint("eot_kbm_finish_prompt"));
    return h;
  }();
  return host;
}

}

namespace eot::ui::mods {

struct Host {
  int32_t (*count)() = nullptr;
  int32_t (*get)(int32_t, eot_mod_info *) = nullptr;
  int32_t (*set_enabled)(const char *, int32_t, char *, int32_t) = nullptr;
  int32_t (*remove)(const char *, char *, int32_t) = nullptr;
  int32_t (*add_begin)() = nullptr;
  int32_t (*import_state)(char *, int32_t) = nullptr;
  void (*import_acknowledge)() = nullptr;
  int32_t (*open_folder)() = nullptr;
  int32_t (*languages)(char *, int32_t) = nullptr;

  bool Bound() const {
    return count && get && set_enabled && remove && add_begin && import_state && import_acknowledge;
  }
};

inline const Host &Api() {
  static const Host host = [] {
    Host h;
    h.count = reinterpret_cast<decltype(h.count)>(HostEntryPoint("eot_mods_count"));
    h.get = reinterpret_cast<decltype(h.get)>(HostEntryPoint("eot_mods_get"));
    h.set_enabled = reinterpret_cast<decltype(h.set_enabled)>(HostEntryPoint("eot_mods_set_enabled"));
    h.remove = reinterpret_cast<decltype(h.remove)>(HostEntryPoint("eot_mods_remove"));
    h.add_begin = reinterpret_cast<decltype(h.add_begin)>(HostEntryPoint("eot_mods_add_begin"));
    h.import_state = reinterpret_cast<decltype(h.import_state)>(HostEntryPoint("eot_mods_import_state"));
    h.import_acknowledge =
        reinterpret_cast<decltype(h.import_acknowledge)>(HostEntryPoint("eot_mods_import_acknowledge"));
    h.open_folder = reinterpret_cast<decltype(h.open_folder)>(HostEntryPoint("eot_mods_open_folder"));
    h.languages = reinterpret_cast<decltype(h.languages)>(HostEntryPoint("eot_mods_languages"));
    return h;
  }();
  return host;
}

}
