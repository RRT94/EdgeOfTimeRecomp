// gamelogic/debug/debug_mode.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "core/export.h"
#include "core/logging.h"

#include "core/memory_helpers.h"
#include "gamelogic/ui/menu_common.h"
#include "gamelogic/ui/hud_api.h"

REXCVAR_DEFINE_BOOL(eot_debug_mode, false, "EdgeOfTime/Config", "Developer level select menu")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__eot_GLInstanciateFrontScreenControl);
REX_EXTERN(__imp__eot_FrontScreenControl_ExitActive);
REX_EXTERN(__imp__eot_GLInstanciateHUDLevelSelect);
REX_EXTERN(__imp__eot_GLInstanciateHUDDebugLevelSelection);
REX_EXTERN(__imp__eot_HUDDebugLevelSelection_Update);
REX_EXTERN(__imp__eot_GameStateManagerSM2011_ReturnToFrontscreen);

namespace {

namespace hud = eot::ui::hud;

constexpr uint32_t kInputApi = 0x883CA224;
constexpr uint32_t kInputQuery = 4;
constexpr uint32_t kActionBack = 10;
constexpr uint32_t kJustPressed = 2;
constexpr uint32_t kFrontScreenMainMenu = 2;

uint32_t CallGuest(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3, uint32_t r4) {
  auto *state = rex::system::kernel_state();
  auto *dispatcher = state ? state->function_dispatcher() : nullptr;
  PPCFunc *fn = dispatcher ? dispatcher->GetFunction(addr) : nullptr;
  if (!fn)
    return 0;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  fn(call, base);
  return call.r3.u32;
}

bool DebugModeEnabled() { return REXCVAR_GET(eot_debug_mode); }

void AnnounceDebugMode() {
  static bool told = false;
  if (told || !DebugModeEnabled())
    return;
  told = true;
  if (auto *tell = reinterpret_cast<void (*)(int32_t)>(eot::HostEntryPoint("eot_debug_mode_active")))
    tell(1);
  EOT_INFO("[debug] debug mode: F5 flies the camera, F6 freezes the scene, F9 steps a frame, F10 replays eot_debug_script; the SDK's own overlays (F3, F4, F7) answer only in this mode");
}

bool BackPressed(const PPCContext &ctx, uint8_t *base) {
  const uint32_t api = eot::mem::load<uint32_t>(kInputApi);
  const uint32_t query = api ? eot::mem::load<uint32_t>(api + kInputQuery) : 0;
  return query && CallGuest(ctx, base, query, kActionBack, kJustPressed) != 0;
}

bool g_returning = false;

uint32_t g_front_screen = 0;

constexpr uint32_t kPageInputMask = 116;
constexpr uint32_t kNoMask = 0xFFFFFFFFu;

constexpr uint32_t kMenuBarCrc = 0x7F85723Du;

constexpr uint32_t kZoneCount = 3;
constexpr uint32_t kMaskArgs = 8;
uint32_t g_mask_args = 0;

}

REX_HOOK_RAW(eot_GLInstanciateFrontScreenControl) {
  __imp__eot_GLInstanciateFrontScreenControl(ctx, base);
  g_front_screen = ctx.r3.u32;
  AnnounceDebugMode();
}

REX_HOOK_RAW(eot_GLInstanciateHUDLevelSelect) {
  AnnounceDebugMode();
  if (!DebugModeEnabled()) {
    __imp__eot_GLInstanciateHUDLevelSelect(ctx, base);
    return;
  }
  g_returning = false;
  __imp__eot_GLInstanciateHUDDebugLevelSelection(ctx, base);
}

REX_HOOK_RAW(eot_HUDDebugLevelSelection_Update) {
  if (DebugModeEnabled() && !g_returning && BackPressed(ctx, base)) {
    g_returning = true;
    PPCContext call = ctx;
    call.r3.u32 = kFrontScreenMainMenu;
    call.r4.u32 = 0;
    __imp__eot_GameStateManagerSM2011_ReturnToFrontscreen(call, base);
    return;
  }
  const uint32_t page = ctx.r3.u32;
  const uint32_t held = eot::mem::load<uint32_t>(page + kPageInputMask);
  __imp__eot_HUDDebugLevelSelection_Update(ctx, base);
  if (held != kNoMask && eot::mem::load<uint32_t>(page + kPageInputMask) == kNoMask) {
    if (g_front_screen) {
      PPCContext exit = ctx;
      exit.r3.u32 = g_front_screen;
      __imp__eot_FrontScreenControl_ExitActive(exit, base);
    }
    const uint32_t bar = hud::Find(ctx, base, kMenuBarCrc);
    if (bar != hud::kNoWindow)
      hud::Call(ctx, base, hud::kWndRemoveFlags, bar, 1);
    if (!g_mask_args)
      g_mask_args = eot::ui::AllocGuest(ctx, base, kMaskArgs);
    if (g_mask_args)
      for (uint32_t zone = 0; zone < kZoneCount; ++zone)
        eot::ui::SendPromptMask(ctx, base, zone, 0, g_mask_args);
  }
}

REX_EXTERN(__imp__eot_BaseAI_SetNextState);             // (object r3, state r4, arg r5)
REX_EXTERN(__imp__eot_BaseAI_PlayAnimSet);
REX_EXTERN(__imp__eot_BaseAI_ActivateAttackCol);        // (object r3, on r4, primitive CRC r5)
REX_EXTERN(__imp__eot_BaseAntiVenom_EnableChargeColPrim); // (on r3), on the object being updated
REX_EXTERN(__imp__eot_BaseHeroSM_EnterHurt);            // (hero r3, kind r4)

namespace {
using eot::mem::load;

constexpr uint32_t kAntiVenomVtables[] = {0x8808C5E0, 0x8808C708, 0x8808C348, 0x8808C498};
constexpr uint32_t kAnimTable = 888;
constexpr uint32_t kAnimCount = 892;

bool IsAntiVenom(uint32_t object) {
  if (!object)
    return false;
  const uint32_t vtable = load<uint32_t>(object);
  for (uint32_t v : kAntiVenomVtables)
    if (vtable == v)
      return true;
  return false;
}

uint32_t AnimHandle(uint32_t object, uint32_t index) {
  if (index >= load<uint32_t>(object + kAnimCount))
    return 0xFFFFFFFFu;
  return load<uint32_t>(load<uint32_t>(object + kAnimTable) + 4 * index);
}
}

REX_HOOK_RAW(eot_BaseAI_SetNextState) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_DEBUG("[av] {:#x} state -> {} (arg {:#x})", object, ctx.r4.u32, ctx.r5.u32);
  __imp__eot_BaseAI_SetNextState(ctx, base);
}

REX_HOOK_RAW(eot_BaseAI_PlayAnimSet) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_DEBUG("[av] {:#x} anim {} (handle {:#x}, flags {}, blend {:.2f}, speed {:.2f}, start {:.2f})", object,
              ctx.r4.u32, AnimHandle(object, ctx.r4.u32), ctx.r5.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64);
  __imp__eot_BaseAI_PlayAnimSet(ctx, base);
}

REX_HOOK_RAW(eot_BaseAI_ActivateAttackCol) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_DEBUG("[av] {:#x} attack collision {} (primitive {:#010x})", object, (ctx.r4.u32 & 0xFF) ? "ON" : "off",
              ctx.r5.u32);
  __imp__eot_BaseAI_ActivateAttackCol(ctx, base);
}

REX_HOOK_RAW(eot_BaseAntiVenom_EnableChargeColPrim) {
  EOT_DEBUG("[av] charge collision {}", (ctx.r3.u32 & 0xFF) ? "ON" : "off");
  __imp__eot_BaseAntiVenom_EnableChargeColPrim(ctx, base);
}

REX_HOOK_RAW(eot_BaseHeroSM_EnterHurt) {
  EOT_DEBUG("[av] hero {:#x} hurt (kind {})", ctx.r3.u32, ctx.r4.u32);
  __imp__eot_BaseHeroSM_EnterHurt(ctx, base);
}
