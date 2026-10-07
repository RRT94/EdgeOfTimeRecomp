// goliath/web_color.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GLAPIGraphics_WebAdd);
REX_EXTERN(__imp__eot_GLAPIGraphics_WebSetColor); // (web r3, float4 RGBA r4)
REX_EXTERN(__imp__eot_GLAPIGraphics_WebGetColor); // (float4 out r3, web r4)
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

REXCVAR_DEFINE_STRING(eot_web_color, "", "EdgeOfTime/Graphics", "Web color, hex or name");

namespace {

constexpr uint32_t kNoWeb = 0xFFFFFFFFu;

struct Colour {
  float r = 1.0f, g = 1.0f, b = 1.0f;
};

std::mutex g_colour_mutex;
Colour g_colour;
std::atomic<bool> g_colour_set{false};
std::atomic<bool> g_colour_stale{true};
std::atomic<bool> g_callback{false};
uint32_t g_scratch = 0;

bool ParseColour(const std::string &text, Colour &out) {
  struct Named {
    const char *name;
    uint32_t rgb;
  };
  static const Named kNamed[] = {{"orange", 0xFF8000}, {"red", 0xFF2010},   {"yellow", 0xFFE000}, {"green", 0x30FF40},
                                 {"cyan", 0x20E0FF},   {"blue", 0x2050FF},  {"purple", 0xA030FF}, {"pink", 0xFF60C0},
                                 {"white", 0xFFFFFF},  {"black", 0x000000}};
  std::string t;
  for (char c : text)
    if (c != ' ' && c != '#')
      t.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  if (t.empty())
    return false;
  uint32_t rgb = 0;
  bool found = false;
  for (const Named &n : kNamed)
    if (t == n.name) {
      rgb = n.rgb;
      found = true;
    }
  if (!found) {
    if (t.size() != 6 || t.find_first_not_of("0123456789abcdef") != std::string::npos) {
      EOT_WARN("[web] eot_web_color '{}' is neither RRGGBB nor a colour name; the game's colour stays", text);
      return false;
    }
    rgb = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
  }
  out.r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
  out.g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
  out.b = static_cast<float>(rgb & 0xFF) / 255.0f;
  return true;
}

void Refresh() {
  if (!g_callback.exchange(true)) {
    rex::cvar::RegisterChangeCallback("eot_web_color", [](std::string_view, std::string_view) {
      g_colour_stale.store(true, std::memory_order_release);
    });
  }
  if (!g_colour_stale.exchange(false))
    return;
  Colour c;
  const std::string text = REXCVAR_GET(eot_web_color);
  const bool set = ParseColour(text, c);
  {
    std::lock_guard lock(g_colour_mutex);
    g_colour = c;
  }
  g_colour_set.store(set, std::memory_order_release);
  if (set)
    EOT_INFO("[web] webs coloured {} ({:.2f} {:.2f} {:.2f})", text, c.r, c.g, c.b);
}

void Recolour(uint32_t float4) {
  Colour c;
  {
    std::lock_guard lock(g_colour_mutex);
    c = g_colour;
  }
  eot::mem::store<float>(float4 + 0, c.r);
  eot::mem::store<float>(float4 + 4, c.g);
  eot::mem::store<float>(float4 + 8, c.b);
}

uint32_t Scratch(const PPCContext &ctx, uint8_t *base) {
  if (!g_scratch) {
    PPCContext call = ctx;
    call.r3.u32 = 16;
    call.r4.u32 = 16;
    call.r5.u32 = 0xFFFFFFFFu;
    call.r6.u32 = 0;
    __imp__eot_MMMemoryMgr_Alloc(call, base);
    g_scratch = call.r3.u32;
  }
  return g_scratch;
}

}

REX_HOOK_RAW(eot_GLAPIGraphics_WebSetColor) {
  Refresh();
  if (g_colour_set.load(std::memory_order_acquire) && ctx.r3.u32 != kNoWeb && ctx.r4.u32)
    Recolour(ctx.r4.u32);
  __imp__eot_GLAPIGraphics_WebSetColor(ctx, base);
}

REX_HOOK_RAW(eot_GLAPIGraphics_WebAdd) {
  Refresh();
  __imp__eot_GLAPIGraphics_WebAdd(ctx, base);
  const uint32_t web = ctx.r3.u32;
  if (web == kNoWeb || !g_colour_set.load(std::memory_order_acquire))
    return;
  const uint32_t scratch = Scratch(ctx, base);
  if (!scratch)
    return;
  PPCContext get = ctx;
  get.r3.u32 = scratch;
  get.r4.u32 = web;
  __imp__eot_GLAPIGraphics_WebGetColor(get, base);
  Recolour(scratch);
  PPCContext set = ctx;
  set.r3.u32 = web;
  set.r4.u32 = scratch;
  __imp__eot_GLAPIGraphics_WebSetColor(set, base);
  ctx.r3.u32 = web;
}

REX_EXTERN(__imp__eot_GLAPI3dObj_MaterialAnimPlay);
REX_EXTERN(__imp__eot_GLAPI3dObj_MaterialAnimStop); // (object handle r3, r4, material r5)
REX_EXTERN(__imp__eot_3dObj_MaterialAnimStart);

namespace {
using eot::mem::load;

constexpr uint32_t kFxObject = 0x883C29A8;
constexpr uint32_t kFxBodyMaterial = 0x883C29A8 + 12;
constexpr uint32_t kNone = 0xFFFFFFFFu;

constexpr uint32_t kObjectRenderData = 340;
constexpr uint32_t kRenderDataGeometry = 48;
constexpr uint32_t kGeometryMaterials = 84;
constexpr uint32_t kGeometryMaterialCount = 108;
constexpr uint32_t kMaterialSize = 960;
constexpr uint32_t kMaterialAnims = 20;
constexpr uint32_t kMaterialAnimCount = 24;
constexpr uint32_t kMaterialAnimNames = 28;
constexpr uint32_t kMaterialAnimNameCount = 32;
constexpr uint32_t kMaxMaterials = 32;

bool g_mirrorNextStart = false;
uint32_t g_mirroredObject = 0;
uint32_t g_mirroredMask = 0;

uint32_t AnimName(uint32_t material, uint32_t index) {
  const uint32_t names = load<uint32_t>(material + kMaterialAnimNames);
  const uint32_t count = load<uint16_t>(material + kMaterialAnimNameCount);
  for (uint32_t i = 0; names && i < count; ++i)
    if (load<uint32_t>(names + 8 * i + 4) == index)
      return load<uint32_t>(names + 8 * i);
  return 0;
}

uint32_t AnimIndex(uint32_t material, uint32_t crc) {
  const uint32_t names = load<uint32_t>(material + kMaterialAnimNames);
  const uint32_t count = load<uint16_t>(material + kMaterialAnimNameCount);
  for (uint32_t i = 0; names && i < count; ++i)
    if (load<uint32_t>(names + 8 * i) == crc)
      return load<uint32_t>(names + 8 * i + 4);
  return kNone;
}
}

REX_HOOK_RAW(eot_GLAPI3dObj_MaterialAnimPlay) {
  const uint32_t object = ctx.r3.u32, material = ctx.r5.u32;
  g_mirrorNextStart = material != kNone && object == load<uint32_t>(kFxObject) && material == load<uint32_t>(kFxBodyMaterial);
  __imp__eot_GLAPI3dObj_MaterialAnimPlay(ctx, base);
  g_mirrorNextStart = false;
}

REX_HOOK_RAW(eot_3dObj_MaterialAnimStart) {
  const PPCContext entry = ctx;
  __imp__eot_3dObj_MaterialAnimStart(ctx, base);
  if (!g_mirrorNextStart)
    return;
  g_mirrorNextStart = false;
  const uint32_t object = entry.r3.u32, material = entry.r4.u32, handle = entry.r5.u32;
  const uint32_t data = load<uint32_t>(object + kObjectRenderData);
  const uint32_t geometry = data ? load<uint32_t>(data + kRenderDataGeometry) : 0;
  if (!geometry)
    return;
  const uint32_t count = load<uint32_t>(geometry + kGeometryMaterialCount);
  const uint32_t records = load<uint32_t>(geometry + kGeometryMaterials);
  if (!records || count > kMaxMaterials || material >= count)
    return;
  const uint32_t name = AnimName(records + kMaterialSize * material, handle & 0xFFFF);
  if (!name)
    return;
  g_mirroredObject = load<uint32_t>(kFxObject);
  g_mirroredMask = 0;
  for (uint32_t m = 0; m < count; ++m) {
    if (m == material)
      continue;
    const uint32_t record = records + kMaterialSize * m;
    const uint32_t index = AnimIndex(record, name);
    if (index == kNone || index >= load<uint32_t>(record + kMaterialAnimCount))
      continue;
    const uint32_t anim = load<uint32_t>(load<uint32_t>(record + kMaterialAnims) + 4 * index);
    if (!anim)
      continue;
    PPCContext call = entry;
    call.r4.u32 = m;
    call.r5.u32 = (m << 16) | index;
    call.r6.u32 = anim;
    __imp__eot_3dObj_MaterialAnimStart(call, base);
    g_mirroredMask |= 1u << m;
  }
  if (g_mirroredMask)
    EOT_DEBUG("[suit] body animation {:08x} mirrored from material {} to mask {:#x}", name, material, g_mirroredMask);
}

REX_HOOK_RAW(eot_GLAPI3dObj_MaterialAnimStop) {
  const PPCContext entry = ctx;
  const uint32_t object = entry.r3.u32, material = entry.r5.u32;
  __imp__eot_GLAPI3dObj_MaterialAnimStop(ctx, base);
  if (!g_mirroredMask || material == kNone || object != g_mirroredObject || material != load<uint32_t>(kFxBodyMaterial))
    return;
  for (uint32_t m = 0; m < kMaxMaterials; ++m) {
    if (!(g_mirroredMask & (1u << m)))
      continue;
    PPCContext call = entry;
    call.r5.u32 = m;
    __imp__eot_GLAPI3dObj_MaterialAnimStop(call, base);
  }
  g_mirroredMask = 0;
}
