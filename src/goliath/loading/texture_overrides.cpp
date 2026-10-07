// goliath/loading/texture_overrides.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/loading/texture_overrides.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <algorithm>
#include <chrono>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <format>
#include <vector>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/button_glyphs.h"
#include "goliath/text/glyph_pages.h"
#include "goliath/ui/name_crc.h"
#include "platform/user_dirs.h"
#include "gpu/gpu_timing.h"

REX_EXTERN(__imp__eot_RZTexture_TextureReplace); // (record r3, replacement r4)
REX_EXTERN(__imp__eot_PKPackageMgrBC_Update);
REX_EXTERN(__imp__eot_GLAPIResource_LoadDiscardableData);   // (handle r3): one more reference on its data
REX_EXTERN(__imp__eot_GLAPIResource_UnloadDiscardableData); // (handle r3): one reference less

REXCVAR_DEFINE_BOOL(eot_texture_overrides, true, "EdgeOfTime/Config", "Use our replacement textures");
REXCVAR_DEFINE_BOOL(eot_antivenom_normals, true, "EdgeOfTime/Config", "Anti-Venom remastered normal maps");
REXCVAR_DEFINE_BOOL(eot_suits_remaster, true, "EdgeOfTime/Config", "Use remastered suit textures");

namespace eot::loading {
namespace {

constexpr uint32_t kRefBlockBytes = 512;

struct Override {
  const char *retail;
  const char *replacement;
  const char *font;
  const char *glyphs;
  const char *language;
  const char *cvar;
  const char *set;
  uint32_t retailCrc;
  uint32_t replacementCrc;
  uint32_t fontCrc;
  bool wide;
};

constexpr Override Make(const char *retail, const char *replacement, const char *font, const char *glyphs = nullptr,
                        const char *language = nullptr, const char *cvar = nullptr) {
  return {retail, replacement,
          font,   glyphs,
          language, cvar,
          nullptr, eot::ui::NameCrc(retail),
          eot::ui::NameCrc(replacement), font ? eot::ui::NameCrc(font) : 0u,
          false};
}

constexpr Override ForSet(const char *retail, const char *replacement, const char *set, bool wide = false) {
  return {retail,  replacement,
          nullptr, nullptr,
          nullptr, nullptr,
          set,     eot::ui::NameCrc(retail),
          eot::ui::NameCrc(replacement), 0u,
          wide};
}

constexpr Override kOverrides[] = {
    Make("TempusGothic_texture_0", "Reeot_Font_TempusGothic_4x", "TempusGothic", "GlyphsTempusGothic"),
    Make("SansaCon-UltraBlack_texture_0", "Reeot_Font_SansaCon_4x", "SansaCon", "GlyphsSansaCon"),
    Make("SMA_SMAmazingState00_D", "Reeot_SMA_State00_D", nullptr),
    Make("SMA_SMAmazingState00_N", "Reeot_SMA_State00_N", nullptr),
    Make("SMA_SMAmazingState00_S", "Reeot_SMA_State00_S", nullptr),
    Make("SMA_SMAmazingState03_D", "Reeot_SMA_State03_D", nullptr),
    Make("SMA_SMAmazingState03_N", "Reeot_SMA_State03_N", nullptr),
    Make("SMA_SMAmazingState03_S", "Reeot_SMA_State03_S", nullptr),
    Make("SMA_SMAmazingState05_D", "Reeot_SMA_State05_D", nullptr),
    Make("SMA_SMAmazingState05_N", "Reeot_SMA_State05_N", nullptr),
    Make("SMA_SMAmazingState05_S", "Reeot_SMA_State05_S", nullptr),
    Make("SMA_SMAmazingState07_D", "Reeot_SMA_State07_D", nullptr),
    Make("SMA_SMAmazingState07_N", "Reeot_SMA_State07_N", nullptr),
    Make("SMA_SMAmazingState07_S", "Reeot_SMA_State07_S", nullptr),
    Make("SMA_AntiVenom_D", "Reeot_AntiVenom_D", nullptr),
    Make("SMA_AntiVenom_N", "Reeot_AntiVenom_N", nullptr, nullptr, nullptr, "eot_antivenom_normals"),
    Make("SMA_MassiveAntiVenom_D", "Reeot_AntiVenomMassive_D", nullptr),
    Make("SMA_MassiveAntiVenom_N", "Reeot_AntiVenomMassive_N", nullptr, nullptr, nullptr,
         "eot_antivenom_normals"),
    Make("SM99_Spiderman_D", "Reeot_SM2099Body_D", nullptr),
    Make("SM99_Spiderman_N", "Reeot_SM2099Body_N", nullptr),
    Make("SM99_Spiderman_S", "Reeot_SM2099Body_S", nullptr),
    Make("SMA_MonsterOck_D", "Reeot_MonsterOck_D", nullptr),
    Make("SMA_MonsterOck_N", "Reeot_MonsterOck_N", nullptr),
    ForSet("SMN_PromptWebPullButton_Xbox_D", "Reeot_MashPrompt_PlayStation_D", "playstation"),
    ForSet("SMN_PromptWebPullButton_Xbox_D", "Reeot_MashPrompt_Switch_D", "switch"),
    ForSet("SMN_PromptWebPullButton_Xbox_D", "Reeot_MashPrompt_Keyboard_D", "keyboard", true),
    ForSet("SMN_PromptWebPullButtonY_D", "Reeot_MashPromptY_PlayStation_D", "playstation"),
    ForSet("SMN_PromptWebPullButtonY_D", "Reeot_MashPrompt_Keyboard_D", "keyboard", true),
};
constexpr uint32_t kOverrideCount = sizeof(kOverrides) / sizeof(kOverrides[0]);
static_assert(kOverrideCount <= 32, "the pending masks are 32 bits wide");

constexpr uint32_t FontMask() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i)
    if (kOverrides[i].font)
      mask |= 1u << i;
  return mask;
}
constexpr uint32_t kFontMask = FontMask();
constexpr uint32_t PromptMask() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i)
    if (kOverrides[i].set)
      mask |= 1u << i;
  return mask;
}
constexpr uint32_t kPromptMask = PromptMask();
constexpr uint32_t kAllMask = kOverrideCount >= 32 ? ~0u : (1u << kOverrideCount) - 1;
constexpr uint32_t kSuitMask = kAllMask & ~kFontMask & ~kPromptMask;

uint32_t WantedMask() {
  const std::string tag = eot::platform::TranslationTag(rex::cvar::GetFlagByName("eot_language"));
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    const Override &o = kOverrides[i];
    if (o.cvar && !rex::cvar::Query<bool>(o.cvar))
      continue;
    if (o.language) {
      if (tag == o.language)
        mask |= 1u << i;
      continue;
    }
    bool specific = false;
    for (uint32_t k = 0; k < kOverrideCount; ++k)
      specific = specific || (kOverrides[k].language && tag == kOverrides[k].language &&
                              kOverrides[k].retailCrc == o.retailCrc);
    if (!specific)
      mask |= 1u << i;
  }
  return mask;
}

uint32_t g_pending = 0;
uint32_t g_wanted = 0;
uint32_t g_off = 0;
bool g_suits_on = true;
bool g_pending_chosen = false;
std::recursive_mutex g_apply_mutex;
uint32_t g_requested = 0;
uint32_t g_waiting_data = 0;
uint32_t g_swapped[kOverrideCount] = {};
uint32_t g_held[kOverrideCount] = {};
// The render thread still executes last frame's commands when the game stops wanting a texture,
// so a swapped suit is let go only once the retail has gone unwanted on several checks in a row.
constexpr uint32_t kReleaseAfterChecks = 4;
uint32_t g_unwanted[kOverrideCount] = {};
constexpr uint32_t kDataReferences = 36;
uint32_t g_ticks = 0;
constexpr uint32_t kMaxTicks = 6000;

void RepointFontAtlas(const PPCContext &ctx, uint8_t *base, const Override &o, uint32_t retail,
                      uint32_t replacement) {
  const uint32_t font = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeFont, o.fontCrc));
  if (!font)
    return;
  uint32_t patched = 0;
  const auto rewrite = [&](uint32_t block) {
    for (uint32_t off = 0; off < kRefBlockBytes; off += 4) {
      const uint32_t v = eot::mem::load<uint32_t>(block + off);
      if (v == o.retailCrc) {
        eot::mem::store<uint32_t>(block + off, o.replacementCrc);
        ++patched;
      } else if (v == retail) {
        eot::mem::store<uint32_t>(block + off, replacement);
        ++patched;
      }
    }
  };
  rewrite(font);
  for (uint32_t off = 0; off < 256; off += 4) {
    const uint32_t ptr = eot::mem::load<uint32_t>(font + off);
    if (ptr >= 0xE0000000u && ptr < 0xF0000000u)
      rewrite(ptr);
  }
  if (o.glyphs)
    eot::text::InstallGlyphs(ctx, base, font, o.glyphs, o.font);
  ReleaseResource(ctx, base, font);
  EOT_INFO("[tex] {}'s atlas reference now names {} ({} word(s))", o.font, o.replacement, patched);
}

enum class Try { Applied, NoRetail, NotReady };

void TextureReplace(const PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t replacement) {
  PPCContext call = ctx;
  call.r3.u32 = record;
  call.r4.u32 = replacement;
  __imp__eot_RZTexture_TextureReplace(call, base);
}

void LoadData(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  PPCContext call = ctx;
  call.r3.u32 = handle;
  __imp__eot_GLAPIResource_LoadDiscardableData(call, base);
}

void UnloadData(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  PPCContext call = ctx;
  call.r3.u32 = handle;
  __imp__eot_GLAPIResource_UnloadDiscardableData(call, base);
}

bool ReleaseSuit(const PPCContext &ctx, uint8_t *base, uint32_t i) {
  const bool had = g_swapped[i] || g_held[i];
  if (g_swapped[i]) {
    const uint32_t retail = AcquireResource(ctx, base, g_swapped[i]);
    if (retail) {
      TextureReplace(ctx, base, retail, 0);
      ReleaseResource(ctx, base, retail);
    }
    g_swapped[i] = 0;
  }
  if (g_held[i]) {
    UnloadData(ctx, base, g_held[i]);
    g_held[i] = 0;
  }
  return had;
}

void TrackSuit(const PPCContext &ctx, uint8_t *base, uint32_t i) {
  const Override &o = kOverrides[i];
  const uint32_t retailHandle = FindResourceFromCrc(ctx, base, kTypeTexture, o.retailCrc);
  const uint32_t retail = AcquireResource(ctx, base, retailHandle);
  const bool wanted =
      retail && (eot::mem::load<uint16_t>(retail + kDataReferences) != 0 || ResourceResident(retail));
  if (!wanted) {
    if ((g_swapped[i] || g_held[i]) && ++g_unwanted[i] < kReleaseAfterChecks) {
      ReleaseResource(ctx, base, retail);
      return;
    }
    g_unwanted[i] = 0;
    if (ReleaseSuit(ctx, base, i))
      EOT_DEBUG("[tex] {} let go with {}", o.replacement, o.retail);
    ReleaseResource(ctx, base, retail);
    return;
  }
  g_unwanted[i] = 0;
  if (!g_held[i]) {
    const uint32_t replacementHandle = FindResourceFromCrc(ctx, base, kTypeTexture, o.replacementCrc);
    if (replacementHandle) {
      LoadData(ctx, base, replacementHandle);
      g_held[i] = replacementHandle;
      EOT_DEBUG("[tex] {} loading with {}", o.replacement, o.retail);
    }
  }
  if (g_held[i] && g_swapped[i] != retailHandle) {
    const uint32_t replacement = AcquireResource(ctx, base, g_held[i]);
    const uint32_t descriptor =
        replacement && ResourceResident(replacement) ? TextureDescriptor(ctx, base, replacement) : 0;
    if (descriptor) {
      TextureReplace(ctx, base, retail, replacement);
      g_swapped[i] = retailHandle;
      EOT_DEBUG("[tex] {} -> {} ({:#x} -> {:#x}, descriptor {:#x})", o.retail, o.replacement, retail,
                replacement, descriptor);
    }
    ReleaseResource(ctx, base, replacement);
  }
  ReleaseResource(ctx, base, retail);
}

void TrackSuits(const PPCContext &ctx, uint8_t *base) {
  const uint32_t mask = g_wanted & kSuitMask & ~g_off;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (mask & (1u << i))
      TrackSuit(ctx, base, i);
  }
}

void TrackPromptArt(const PPCContext &ctx, uint8_t *base) {
  const uint32_t mask = g_wanted & kPromptMask;
  if (!mask)
    return;
  const std::string_view set = eot::controller::ActiveGlyphSet();
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if ((mask & (1u << i)) && set != kOverrides[i].set && ReleaseSuit(ctx, base, i))
      EOT_DEBUG("[tex] {} let go: the prompts draw the {} set", kOverrides[i].replacement, set);
  }
  uint32_t wide[2] = {0, 0};
  uint32_t n = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (!(mask & (1u << i)) || set != kOverrides[i].set)
      continue;
    TrackSuit(ctx, base, i);
    if (g_swapped[i] && kOverrides[i].wide && n < 2)
      wide[n++] = kOverrides[i].retailCrc;
  }
  eot::goliath::SetWidePromptSheets(wide[0], wide[1]);
}

void RevertSuits(const PPCContext &ctx, uint8_t *base) {
  uint32_t done = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (!(g_wanted & kSuitMask & (1u << i)))
      continue;
    g_off |= 1u << i;
    if (ReleaseSuit(ctx, base, i))
      ++done;
  }
  EOT_INFO("[tex] suits remaster off: {} texture(s) back to the console's own", done);
}

Try TryApply(const PPCContext &ctx, uint8_t *base, uint32_t i) {
  const Override &o = kOverrides[i];
  const uint32_t retailHandle = FindResourceFromCrc(ctx, base, kTypeTexture, o.retailCrc);
  if (!retailHandle)
    return Try::NoRetail;
  const uint32_t replacement = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeTexture, o.replacementCrc));
  if (!replacement)
    return Try::NotReady;

  const uint32_t descriptor = ResourceResident(replacement) ? TextureDescriptor(ctx, base, replacement) : 0;
  if (!descriptor) {
    if (!(g_requested & (1u << i))) {
      g_requested |= 1u << i;
      RequestResourceLoad(ctx, base, replacement);
      EOT_DEBUG("[tex] {} asked to load; the swap waits for its data", o.replacement);
    }
    ReleaseResource(ctx, base, replacement);
    return Try::NotReady;
  }
  if (o.glyphs && !eot::text::GlyphTableReady(o.glyphs)) {
    ReleaseResource(ctx, base, replacement);
    return Try::NotReady;
  }

  const uint32_t retail = AcquireResource(ctx, base, retailHandle);
  if (retail) {
    PPCContext call = ctx;
    call.r3.u32 = retail;
    call.r4.u32 = replacement;
    __imp__eot_RZTexture_TextureReplace(call, base);
    EOT_DEBUG("[tex] {} -> {} ({:#x} -> {:#x}, descriptor {:#x})", o.retail, o.replacement, retail, replacement,
              descriptor);
    if (o.font)
      RepointFontAtlas(ctx, base, o, retail, replacement);
    g_swapped[i] = retailHandle;
  }
  ReleaseResource(ctx, base, retail);
  ReleaseResource(ctx, base, replacement);
  return retail ? Try::Applied : Try::NotReady;
}

}

void ApplyTextureOverrides(const PPCContext &ctx, uint8_t *base) {
  std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
  if (!g_pending_chosen) {
    g_pending_chosen = true;
    g_wanted = REXCVAR_GET(eot_texture_overrides) ? WantedMask() : 0;
    g_suits_on = REXCVAR_GET(eot_suits_remaster);
    g_off = g_suits_on ? 0 : (g_wanted & kSuitMask);
    g_pending = g_wanted & kFontMask;
    EOT_DEBUG("[tex] overrides wanted: {:#x} of {}{}", g_wanted, kOverrideCount,
              g_suits_on ? "" : " (the suits are switched off)");
  }
  if (!g_pending)
    return;
  g_waiting_data = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (!(g_pending & (1u << i)))
      continue;
    switch (TryApply(ctx, base, i)) {
    case Try::Applied:
      g_pending &= ~(1u << i);
      break;
    case Try::NotReady:
      g_waiting_data |= 1u << i;
      break;
    case Try::NoRetail:
      break;
    }
  }
}

void TextureOverridesPackageMounted(const PPCContext &ctx, uint8_t *base) {
  std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
  if (!g_pending_chosen)
    return;
  if (g_pending)
    ApplyTextureOverrides(ctx, base);
}

bool TextureOverridesSettled() { return g_pending_chosen && !(g_pending & kFontMask); }

namespace {

void SuitsRemasterTick(const PPCContext &ctx, uint8_t *base) {
  if (!g_pending_chosen || REXCVAR_GET(eot_suits_remaster) == g_suits_on)
    return;
  std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
  g_suits_on = !g_suits_on;
  if (g_suits_on) {
    g_off = 0;
    EOT_INFO("[tex] suits remaster on: the port's art follows the retail textures again");
  } else {
    RevertSuits(ctx, base);
  }
}

}

}

REX_HOOK_RAW(eot_PKPackageMgrBC_Update) {
  __imp__eot_PKPackageMgrBC_Update(ctx, base);
  using namespace eot::loading;
  SuitsRemasterTick(ctx, base);
  static uint32_t track_tick = 0;
  if (g_pending_chosen && (++track_tick & 3) == 0) {
    std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
    TrackSuits(ctx, base);
    TrackPromptArt(ctx, base);
  }
  if (g_pending_chosen && g_pending) {
    std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
    ApplyTextureOverrides(ctx, base);
    if (g_waiting_data && ++g_ticks > kMaxTicks) {
      EOT_WARN("[tex] {} override(s) never became ready; giving up", __builtin_popcount(g_waiting_data));
      g_pending &= ~g_waiting_data;
      g_waiting_data = 0;
    }
    if (!g_pending)
      EOT_DEBUG("[tex] overrides in place after {} manager ticks", g_ticks);
  }
  eot::controller::ButtonGlyphsTick(ctx, base);
  HeapCensusTick(ctx, base);
}

REX_EXTERN(__imp__eot_TextureAsset_LoadInitialResidentDescriptor); // (texture r3, chunk r4, stream r5, r6)
REX_EXTERN(__imp__eot_RZTexture_RefreshData);                     // (texture r3, chunk r4, stream r5)
REX_EXTERN(__imp__eot_RZTexture_FreeResidentDescriptors);         // (texture r3)
REX_EXTERN(__imp__eot_RZTexture_UnloadDiscardableData);           // (texture r3)

namespace {

constexpr uint32_t kBuiltDescriptor = 0x50;
constexpr uint32_t kOtherDescriptors[2] = {0x54, 0x5C};
constexpr uint32_t kDescriptorHeader = 4;

uint32_t BuiltDescriptor(uint32_t texture) {
  return texture ? eot::mem::load<uint32_t>(texture + kBuiltDescriptor) : 0;
}

uint32_t Header(uint32_t descriptor) {
  return descriptor ? eot::mem::load<uint32_t>(descriptor + kDescriptorHeader) : 0;
}

}

REX_HOOK_RAW(eot_TextureAsset_LoadInitialResidentDescriptor) {
  const uint32_t texture = ctx.r3.u32;
  __imp__eot_TextureAsset_LoadInitialResidentDescriptor(ctx, base);
  eot::gpu::NoteTextureResident(Header(BuiltDescriptor(texture)));
}

REX_HOOK_RAW(eot_RZTexture_RefreshData) {
  const uint32_t texture = ctx.r3.u32;
  const uint32_t before = BuiltDescriptor(texture);
  __imp__eot_RZTexture_RefreshData(ctx, base);
  const uint32_t after = BuiltDescriptor(texture);
  if (after != before)
    eot::gpu::NoteTextureResident(Header(after));
}

REX_HOOK_RAW(eot_RZTexture_FreeResidentDescriptors) {
  const uint32_t texture = ctx.r3.u32;
  uint32_t headers[3] = {Header(BuiltDescriptor(texture)), 0, 0};
  for (int i = 0; i < 2; ++i)
    headers[i + 1] = texture ? Header(eot::mem::load<uint32_t>(texture + kOtherDescriptors[i])) : 0;
  __imp__eot_RZTexture_FreeResidentDescriptors(ctx, base);
  for (uint32_t header : headers)
    eot::gpu::NoteTextureReleased(header);
}

REX_HOOK_RAW(eot_RZTexture_UnloadDiscardableData) {
  const uint32_t header = Header(BuiltDescriptor(ctx.r3.u32));
  __imp__eot_RZTexture_UnloadDiscardableData(ctx, base);
  eot::gpu::NoteTextureReleased(header);
}

REX_EXTERN(eot_WorkBuf_Lock);   // (section r3, timeout r4)
REX_EXTERN(eot_WorkBuf_Unlock);

namespace eot::loading {
namespace {

constexpr uint32_t kHeapTable = 0x824E61F0;
constexpr uint32_t kMainHeapSlot = 4;
constexpr uint32_t kHeapLock = 12;
constexpr uint32_t kHeapInUse = 32;
constexpr uint32_t kHeapFreeCells = 60;
constexpr uint32_t kSizeClasses = 12;
constexpr uint32_t kBlockNext = 0;
constexpr uint32_t kBlockSize = 12;
constexpr uint32_t kBlockSizeMask = 0x3FFFFFFF;
constexpr uint32_t kMaxBlocks = 1u << 20;
constexpr auto kInterval = std::chrono::seconds(30);

struct Census {
  uint32_t heap = 0;
  uint32_t in_use = 0;
  uint64_t free = 0;
  uint32_t largest = 0;
  uint32_t blocks = 0;
};

Census Walk(const PPCContext &ctx, uint8_t *base) {
  Census census;
  const uint32_t table = eot::mem::load<uint32_t>(kHeapTable);
  census.heap = table ? eot::mem::load<uint32_t>(table + kMainHeapSlot) : 0;
  if (!census.heap)
    return census;

  PPCContext call = ctx;
  call.r3.u32 = census.heap + kHeapLock;
  call.r4.u32 = 0xFFFFFFFFu;
  eot_WorkBuf_Lock(call, base);
  census.in_use = eot::mem::load<uint32_t>(census.heap + kHeapInUse);
  for (uint32_t size_class = 0; size_class < kSizeClasses && census.blocks < kMaxBlocks; ++size_class) {
    const uint32_t cell = eot::mem::load<uint32_t>(census.heap + kHeapFreeCells + 4 * size_class);
    for (uint32_t block = cell ? eot::mem::load<uint32_t>(cell) : 0; block && census.blocks < kMaxBlocks;
         block = eot::mem::load<uint32_t>(block + kBlockNext)) {
      const uint32_t size = eot::mem::load<uint32_t>(block + kBlockSize) & kBlockSizeMask;
      census.free += size;
      census.largest = std::max(census.largest, size);
      ++census.blocks;
    }
  }
  call = ctx;
  call.r3.u32 = census.heap + kHeapLock;
  eot_WorkBuf_Unlock(call, base);
  return census;
}

uint64_t FreePhysicalBytes() {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  auto *physical = memory ? memory->GetPhysicalHeap() : nullptr;
  return physical ? uint64_t(physical->GetUnreservedPageCount()) * physical->page_size() : 0;
}

std::chrono::steady_clock::time_point g_next{};

}

void HeapCensusTick(const PPCContext &ctx, uint8_t *base) {
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next)
    return;
  g_next = now + kInterval;
  auto *log = ::rex::GetLoggerRaw(::rex::log::eot());
  if (!log || !log->should_log(spdlog::level::debug))
    return;
  const Census census = Walk(ctx, base);
  if (!census.heap)
    return;
  EOT_DEBUG("[mem] heap 0: {} MB in use, {} MB free in {} blocks (largest {} KB); {} MB of physical memory "
            "outside it",
            census.in_use >> 20, census.free >> 20, census.blocks, census.largest >> 10, FreePhysicalBytes() >> 20);
}

}

REX_EXTERN(__imp__eot_XContentCreate);

namespace {

constexpr uint32_t kGdlcCount = 0x8249BCE0;
constexpr uint32_t kGdlcObjects = 0x8249BCE8;
constexpr uint32_t kGdlcMaxObjects = 16;
constexpr uint32_t kObjEntries = 0;
constexpr uint32_t kObjEntryCount = 256;
constexpr uint32_t kObjFlags = 2352;
constexpr uint32_t kObjSlot = 2364;
constexpr uint32_t kObjFileName = 2400;
constexpr uint32_t kObjOpened = 2448;
constexpr uint32_t kEntryId = 0;               // u32 read at file offset 0x18
constexpr uint32_t kEntryPath = 4;
constexpr uint32_t kXContentFileName = 0x108;

constexpr uint32_t kCostumeTable = 0x883DD878;
constexpr uint32_t kCostumeCount = 0x883DDC38;
constexpr uint32_t kCostumeStride = 32;
constexpr uint32_t kCostumeHud = 28;
constexpr uint32_t kCostumeCapacity = 30;

std::string GuestString(uint32_t va, uint32_t max) {
  std::string s;
  for (uint32_t i = 0; va && i < max; ++i) {
    const char c = static_cast<char>(eot::mem::load<uint8_t>(va + i));
    if (!c)
      break;
    s.push_back(c);
  }
  return s;
}

std::string DescribeItem(uint32_t index) {
  const uint32_t obj = eot::mem::load<uint32_t>(kGdlcObjects + index * 4);
  if (!obj || !eot::mem::readable(obj, kObjOpened + 2))
    return {};
  const uint32_t flags = eot::mem::load<uint32_t>(obj + kObjFlags);
  const uint32_t count = eot::mem::load<uint32_t>(obj + kObjEntryCount);
  std::string line = std::format("item {} '{}' GDLC{}: flags {:#x} opened {} paks {}", index,
                                 GuestString(obj + kObjFileName, 42), eot::mem::load<uint32_t>(obj + kObjSlot),
                                 flags, eot::mem::load<uint8_t>(obj + kObjOpened), count);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const uint32_t entry = eot::mem::load<uint32_t>(obj + kObjEntries + i * 4);
    if (!entry)
      continue;
    line += std::format("{} id {:#x} '{}'", i ? "," : ":", eot::mem::load<uint32_t>(entry + kEntryId),
                        GuestString(entry + kEntryPath, 256));
  }
  return line;
}

std::vector<std::string> g_items;
std::string g_costumes;
bool g_gamelogic_mapped = false;

}

REX_HOOK_RAW(eot_XContentCreate) {
  const std::string root = GuestString(ctx.r4.u32, 32);
  const std::string file = ctx.r5.u32 ? GuestString(ctx.r5.u32 + kXContentFileName, 42) : std::string();
  __imp__eot_XContentCreate(ctx, base);
  EOT_DEBUG("[dlc] XContentCreate('{}', '{}') -> {:#x}", root, file, ctx.r3.u32);
}

namespace eot::loading {

void DlcTraceTick() {
  const uint32_t count = eot::mem::load<uint32_t>(kGdlcCount);
  if (count <= kGdlcMaxObjects) {
    if (g_items.size() < count)
      g_items.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      std::string line = DescribeItem(i);
      if (line != g_items[i]) {
        if (!line.empty())
          EOT_DEBUG("[dlc] {}", line);
        g_items[i] = std::move(line);
      }
    }
  }

  if (!g_gamelogic_mapped) {
    if (!eot::mem::readable(kCostumeTable, kCostumeStride * kCostumeCapacity) ||
        !eot::mem::readable(kCostumeCount, 4))
      return;
    g_gamelogic_mapped = true;
  }
  const uint32_t costumes = eot::mem::load<uint32_t>(kCostumeCount);
  std::string line;
  for (uint32_t i = 0; i < costumes && i < kCostumeCapacity; ++i) {
    const uint32_t row = kCostumeTable + i * kCostumeStride;
    line += std::format("{}{}{}", i ? " " : "", eot::mem::load<uint32_t>(row),
                        eot::mem::load<uint32_t>(row + kCostumeHud) ? "" : "(no gallery row)");
  }
  if (line != g_costumes) {
    EOT_DEBUG("[dlc] costume table: {} entries{}{}", costumes, line.empty() ? "" : ": ids ", line);
    g_costumes = std::move(line);
  }
}

}
