// goliath/loading/texture_overrides.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/hook.h>
#include <cstdint>
#include "core/memory_helpers.h"

namespace eot::loading {

void ApplyTextureOverrides(const PPCContext &ctx, uint8_t *base);

void TextureOverridesPackageMounted(const PPCContext &ctx, uint8_t *base);

bool TextureOverridesSettled();

}

REX_EXTERN(__imp__eot_GLAPIResource_FindResourceFromCRC);      // (type r3, nameCRC r4) -> handle or -1
REX_EXTERN(__imp__eot_RZResourceMgrBC_GetResourceByHandle);    // (handle r3) -> record, referenced
REX_EXTERN(__imp__eot_RZResource_Release);                     // (record r3)
REX_EXTERN(__imp__eot_RZResourceMgrBC_AcquireDiscardableData); // (record r3, level r4, ownThread r5)
REX_EXTERN(__imp__eot_RZTexture_GetTexture);                   // (record r3) -> descriptor or 0

namespace eot::loading {

constexpr uint32_t kTypeTexture = 4;
constexpr uint32_t kTypeFont = 7;
constexpr uint32_t kNoHandle = 0xFFFFFFFFu;
constexpr uint32_t kResidencyWord = 72;

inline uint32_t FindResourceFromCrc(const PPCContext &ctx, uint8_t *base, uint32_t type, uint32_t crc) {
  PPCContext call = ctx;
  call.r3.u32 = type;
  call.r4.u32 = crc;
  __imp__eot_GLAPIResource_FindResourceFromCRC(call, base);
  return call.r3.u32 == kNoHandle ? 0 : call.r3.u32;
}

inline uint32_t AcquireResource(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  if (!handle)
    return 0;
  PPCContext call = ctx;
  call.r3.u32 = handle;
  __imp__eot_RZResourceMgrBC_GetResourceByHandle(call, base);
  return call.r3.u32;
}

inline void ReleaseResource(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  if (!record)
    return;
  PPCContext call = ctx;
  call.r3.u32 = record;
  __imp__eot_RZResource_Release(call, base);
}

inline uint32_t TextureDescriptor(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  PPCContext call = ctx;
  call.r3.u32 = record;
  __imp__eot_RZTexture_GetTexture(call, base);
  return call.r3.u32;
}

inline bool ResourceResident(uint32_t record) {
  return (eot::mem::load<uint32_t>(record + kResidencyWord) & 1) != 0;
}

inline void RequestResourceLoad(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  PPCContext call = ctx;
  call.r3.u32 = record;
  call.r4.u32 = 1;
  call.r5.u32 = 1;
  __imp__eot_RZResourceMgrBC_AcquireDiscardableData(call, base);
}

}

namespace eot::loading {

void HeapCensusTick(const PPCContext &ctx, uint8_t *base);

}

namespace eot::loading {

void DlcTraceTick();

}
