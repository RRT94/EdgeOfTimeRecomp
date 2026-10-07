// gamelogic/ui/hud_api.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/func.h>

#include "core/memory_helpers.h"

namespace eot::ui::hud {

inline constexpr uint32_t kApiTablePtr = 0x883CA220;

enum Slot : uint32_t {
  kGetParent = 1,
  kWndGetFlags = 2,
  kWndAddFlags = 3,
  kWndRemoveFlags = 4,
  kWndGetPos = 5,
  kWndSetPos = 6,
  kWndSetColors = 11,
  kTextWndSetText = 16,
  kTextWndSetString = 17,
  kWnd2DSetTexture = 23,
  kWnd2DSetUVs = 24,
  kCreateWnd = 40,
  kDestroyWnd = 41,
  kWndGetChild = 43,
  kWndGetSibling = 44,
  kWndSetParent = 53,
  kActivate = 57,
  kIsActive = 58,
  kFindHUDWndFromCRC = 71,
  kTextWndSetStyle = 73,
  kFindStringFromCRC = 86,
  kWndExists = 87,
  kCopyWnd = 90,
};

inline constexpr uint32_t kFlagActive = 0x1;
inline constexpr uint32_t kFlagLive = 0x400;

inline constexpr uint32_t kNoWindow = 0xFFFFFFFFu;

inline uint32_t Entry(uint32_t slot) {
  const uint32_t table = eot::mem::load<uint32_t>(kApiTablePtr);
  return table ? eot::mem::load<uint32_t>(table + (slot - 1) * 4) : 0;
}

inline uint32_t CallAt(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3 = 0, uint32_t r4 = 0,
                       uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0) {
  PPCFunc *fn = addr ? rex::runtime::ResolveIndirectFunction(addr) : nullptr;
  if (!fn)
    return kNoWindow;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  call.r5.u32 = r5;
  call.r6.u32 = r6;
  call.r7.u32 = r7;
  call.r8.u32 = r8;
  fn(call, base);
  return call.r3.u32;
}

inline uint32_t Call(const PPCContext &ctx, uint8_t *base, uint32_t slot, uint32_t r3 = 0, uint32_t r4 = 0,
                     uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0) {
  return CallAt(ctx, base, Entry(slot), r3, r4, r5, r6, r7, r8);
}

inline uint32_t Find(const PPCContext &ctx, uint8_t *base, uint32_t crc) {
  return Call(ctx, base, kFindHUDWndFromCRC, crc);
}

inline void Activate(const PPCContext &ctx, uint8_t *base, uint32_t handle, bool on) {
  if (handle != kNoWindow)
    Call(ctx, base, kActivate, handle, on ? 1u : 0u);
}

inline void SetString(const PPCContext &ctx, uint8_t *base, uint32_t handle, uint32_t stringHandle) {
  if (handle != kNoWindow)
    Call(ctx, base, kTextWndSetString, handle, stringHandle);
}

inline uint32_t Flags(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  return handle == kNoWindow ? 0 : Call(ctx, base, kWndGetFlags, handle);
}

inline uint32_t FindString(const PPCContext &ctx, uint8_t *base, uint32_t nameCrc) {
  const uint32_t handle = Call(ctx, base, kFindStringFromCRC, nameCrc);
  return handle == kNoWindow ? 0 : handle;
}

}
