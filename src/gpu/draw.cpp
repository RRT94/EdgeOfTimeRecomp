// gpu/draw.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/draw.h"

#include <unordered_map>

#include <xxhash.h>

#include "core/profiling.h"

#include "gpu/settings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"
#include "gpu/render_thread.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/textures.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/surfaces.h"
#include "gpu/taa.h"
#include "gpu/trace.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr u32 kPrimTriangleStrip = 6;
constexpr u32 kPrimTriangleFan = 5;
constexpr u32 kPrimRectList = 8;
constexpr u32 kPrimQuadList = 13;
constexpr u32 kPrimLineStrip = 3;

struct Targets {
  GuestSurface *color[4] = {};
  u32 colorCount = 0;
  GuestSurface *depth = nullptr;
  HostTexture *colorImage[4] = {};
  HostTexture *depthImage = nullptr;
  u32 samples = 1;
  bool writesDepthStencil = false;
  bool writesDepth = false;
  bool writesColor = true;
  bool additive = false;
  u32 width = 0;
  u32 height = 0;
  float scale = 1.0f;
  i32 offsetX = 0;
  i32 offsetY = 0;
  velocity::Target *velocity = nullptr;
};

struct ViewportInfo {
  plume::RenderViewport vp;
  plume::RenderRect scissor;
  float posScale[4] = {1, 1, 1, 1};
  float posOffset[4] = {0, 0, 0, 0};
};

void Dropped(const char *why, u64 site) {
  trace::Bump(trace::Counter::DrawDropped);
  u32 n;
  if (DiagShouldLog(site, &n))
    EOT_WARN("[draw] dropped: {} (x{})", why, n + 1);
}

bool ResolveTargets(VideoState &s, DeviceView dev, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va)
      break;
    GuestSurface *surf = GetGuestSurface(s, va);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 packet = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    const u32 bias = (packet >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  const u32 ds_va = dev.U32(dev::kDepthSurface);
  if (ds_va)
    t.depth = GetGuestSurface(s, ds_va);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

bool CaptureTargetWords(DeviceView dev, TargetWords &tw) {
  tw.colorCount = 0;
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va || !ReadSurfaceHeaderWords(va, tw.colorWords[i]))
      break;
    tw.colorVa[i] = va;
    tw.colorInfo[i] = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    tw.colorCount = i + 1;
  }
  tw.depthVa = dev.U32(dev::kDepthSurface);
  if (tw.depthVa && !ReadSurfaceHeaderWords(tw.depthVa, tw.depthWords))
    tw.depthVa = 0;
  return tw.colorCount || tw.depthVa;
}

bool ResolveTargetsFromWords(VideoState &s, const TargetWords &tw, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < tw.colorCount; ++i) {
    GuestSurface *surf = GetGuestSurfaceWords(s, tw.colorVa[i], tw.colorWords[i]);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 bias = (tw.colorInfo[i] >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  if (tw.depthVa)
    t.depth = GetGuestSurfaceWords(s, tw.depthVa, tw.depthWords);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

struct DrawClass {
  bool nullPs = false;
  bool depthTest = false;
  bool depthWrite = false;
  bool stencil = false;
  bool stencilWrite = false;
  bool blend = false;
  bool additive = false;
  bool rect = false;
  bool eightBit = false;
};

DrawClass ClassifyDraw(DeviceView dev, const Targets &t, bool has_ps, bool rect_list) {
  DrawClass c;
  c.nullPs = !has_ps;
  c.rect = rect_list;
  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  c.depthTest = has_ds && (dc & 2);
  c.depthWrite = has_ds && (dc & 4);
  c.stencil = has_ds && (dc & 1);
  if (c.stencil) {
    const u32 front_ops = ((dc >> 11) & 7) | ((dc >> 14) & 7) | ((dc >> 17) & 7);
    const u32 back_ops = (dc & 0x80) ? (((dc >> 23) & 7) | ((dc >> 26) & 7) | ((dc >> 29) & 7)) : 0;
    const u32 write_mask = (dev.U32(dev::kStencilRefMask) >> 16) & 0xFF;
    c.stencilWrite = (front_ops | back_ops) != 0 && write_mask != 0;
  }
  if (t.colorCount) {
    const u32 cc = dev.U32(dev::kColorControl);
    const u32 bc = dev.U32(dev::kBlendControl0);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0;
    c.blend = !(cc & (1u << 5)) && !passthrough;
    c.additive = c.blend && op == 0 && dst == 1;
    c.eightBit = t.color[0]->colorFormat <= 1; // k_8_8_8_8, k_8_8_8_8_GAMMA
  }
  return c;
}

u32 SelectPassSamples(const Targets &t, const DrawClass &c, bool stencil_twin) {
  const u32 ms = t.colorCount ? t.color[0]->host.sampleCount
                              : (t.depth ? t.depth->host.sampleCount : 1u);
  if (ms <= 1)
    return 1;
  const bool ms_depth = t.depth && t.depth->host.sampleCount > 1;
  if (ms_depth && c.stencil && !stencil_twin)
    return ms;
  if (!t.colorCount)
    return ms;
  const GuestSurface &c0 = *t.color[0];
  const bool resolved = c0.content == GuestSurface::Content::Borrowed ||
                        (c0.content == GuestSurface::Content::Drawn &&
                         (c0.contentInSingle || c0.imagesAgree || c0.resolvedSinceDraw));
  if (c.nullPs) {
    if (ms_depth && resolved)
      return 1;
    return ms_depth && !c.depthWrite && stencil_twin ? 1u : ms;
  }
  if (resolved)
    return 1;
  if (ms_depth && c.depthWrite && !c.blend)
    return ms;
  if (!c.rect && !c.additive && !c.nullPs && c.depthTest && !c.blend && !c.eightBit)
    return ms;
  return c0.content == GuestSurface::Content::Drawn ? ms : 1u;
}

bool SelectTargetImages(VideoState &s, Targets &t, const DrawClass &c) {
  u32 samples = SelectPassSamples(t, c, s.stencil_ref_supported);
  if (t.depth && t.colorCount && samples > 1 && t.depth->host.sampleCount != samples)
    samples = 1;
  if (samples > 1) {
    for (u32 i = 0; i < t.colorCount && samples > 1; ++i)
      if (!t.color[i]->host.valid() && !SurfaceMakeMultisampled(s, *t.color[i]))
        samples = 1;
    if (samples > 1 && t.depth && t.depth->host.sampleCount == samples && !t.depth->host.valid() &&
        !SurfaceMakeMultisampled(s, *t.depth))
      samples = 1;
  }
  for (u32 i = 0; i < t.colorCount; ++i) {
    t.colorImage[i] = SurfaceImageForDraw(s, *t.color[i], samples, !c.nullPs);
    if (!t.colorImage[i])
      return false;
  }
  if (t.depth) {
    if (samples == t.depth->host.sampleCount) {
      if (samples > 1 && t.depth->singleDirty && !SurfacePropagateDepthSingle(s, *t.depth))
        return false;
      t.depthImage = &t.depth->host;
      if (!t.colorCount && t.depth->redirectMirror && t.depth->redirectMirror->host.valid()) {
        t.depthImage = &t.depth->redirectMirror->host;
        t.offsetX = t.depth->redirectX;
        t.offsetY = t.depth->redirectY;
      }
    } else {
      t.depthImage = SurfaceDepthSingle(s, *t.depth, c.depthTest || c.stencil || c.depthWrite);
      if (!t.depthImage)
        return false;
    }
  }
  t.samples = samples;
  t.writesDepthStencil = c.depthWrite || (c.stencilWrite && s.stencil_ref_supported);
  t.writesDepth = c.depthWrite;
  t.writesColor = !c.nullPs;
  t.additive = c.additive;
  return true;
}

bool BindImages(VideoState &s, HostTexture *const colors[4], u32 color_count, HostTexture *depth,
                bool init_clear) {
  HostTextureTransition transitions[kMaxPendingTransitions + 5];
  u32 transition_count = 0;
  for (u32 i = 0; i < s.pending_transition_count; ++i)
    transitions[transition_count++] = s.pending_transitions[i];
  s.pending_transition_count = 0;
  for (u32 i = 0; i < color_count; ++i)
    transitions[transition_count++] = {colors[i], plume::RenderTextureLayout::COLOR_WRITE};
  if (depth)
    transitions[transition_count++] = {depth, plume::RenderTextureLayout::DEPTH_WRITE};
  TransitionManyLocked(s, transitions, transition_count);
  if (depth && color_count && colors[0]->sampleCount != depth->sampleCount) {
    u32 n;
    if (DiagShouldLog(0x6C20, &n) && n < 8)
      EOT_WARN("[draw] colour {}x{} ({}x samples) bound with depth {}x{} ({}x samples)",
               colors[0]->width, colors[0]->height, colors[0]->sampleCount, depth->width,
               depth->height, depth->sampleCount);
  }
  bool same_targets = s.bound_draw_targets_valid && s.bound_framebuffer &&
                      s.bound_draw_color_count == color_count &&
                      s.bound_draw_depth == (depth ? depth->texture.get() : nullptr);
  for (u32 i = 0; same_targets && i < color_count; ++i)
    same_targets = s.bound_draw_colors[i] == colors[i]->texture.get();
  if (same_targets) {
    s.perf.framebuffer_cache_hits++;
  } else {
    plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, color_count, depth);
    if (!fb)
      return false;
    if (s.bound_framebuffer != fb) {
      s.command_list->setFramebuffer(fb);
      s.bound_framebuffer = fb;
    }
    s.bound_draw_color_count = color_count;
    for (u32 i = 0; i < 4; ++i)
      s.bound_draw_colors[i] = i < color_count ? colors[i]->texture.get() : nullptr;
    s.bound_draw_depth = depth ? depth->texture.get() : nullptr;
    s.bound_draw_targets_valid = true;
  }
  if (!init_clear)
    return true;
  for (u32 i = 0; i < color_count; ++i) {
    if (!colors[i]->needsClear)
      continue;
    s.command_list->clearColor(i, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
    colors[i]->needsClear = false;
  }
  if (depth && depth->needsClear) {
    s.command_list->clearDepthStencil(true, FormatHasStencil(depth->format), 0.0f, 0, nullptr, 0);
    depth->needsClear = false;
  }
  return true;
}

bool BindTargets(VideoState &s, Targets &t) {
  HostTexture *colors[4] = {};
  for (u32 i = 0; i < t.colorCount; ++i) {
    colors[i] = t.colorImage[i] ? t.colorImage[i] : &t.color[i]->host;
    if (t.writesColor)
      NoteSurfaceDrawn(*t.color[i], *colors[i], false);
    t.color[i]->lastUseFrame = s.guest_frames;
  }
  HostTexture *depth = nullptr;
  if (t.depth) {
    depth = t.depthImage ? t.depthImage : &t.depth->host;
    NoteSurfaceDrawn(*t.depth, *depth, t.writesDepthStencil, t.writesDepth);
    t.depth->lastUseFrame = s.guest_frames;
  }
  u32 color_count = t.colorCount;
  if (t.velocity && color_count < 4)
    colors[color_count++] = &t.velocity->image;
  if (!BindImages(s, colors, color_count, depth, true))
    return false;
  if (t.velocity)
    velocity::BeforeDraw(s, *t.velocity, color_count - 1);
  GpuTimingMark(s, s.command_list,
                GpuTargetCategory(t.width == kGuestRenderWidth && t.height == kGuestRenderHeight,
                                  t.depth != nullptr, t.colorCount,
                                  t.colorCount ? static_cast<u32>(colors[0]->format) : 0u,
                                  t.samples, t.additive));
  return true;
}

ViewportInfo ComputeViewport(DeviceView dev, const Targets &t, float jitter_x, float jitter_y) {
  ViewportInfo v;
  const float rt_w = static_cast<float>(std::max(1u, t.width));
  const float rt_h = static_cast<float>(std::max(1u, t.height));
  const u32 vte = dev.U32(dev::kVteControl);
  const float xs = dev.F32(dev::kVportXScale), xo = dev.F32(dev::kVportXOffset);
  const float ys = dev.F32(dev::kVportYScale), yo = dev.F32(dev::kVportYOffset);
  const float zs = dev.F32(dev::kVportZScale), zo = dev.F32(dev::kVportZOffset);
  const bool xs_en = vte & 1, xo_en = vte & 2, ys_en = vte & 4, yo_en = vte & 8;
  const bool zs_en = vte & 16, zo_en = vte & 32;

  float x0, w, y0, h;
  if (xs_en && xs != 0.0f) {
    w = 2.0f * std::fabs(xs);
    x0 = (xo_en ? xo : 0.0f) - std::fabs(xs);
    v.posScale[0] = xs < 0 ? -1.0f : 1.0f;
  } else {
    x0 = 0.0f;
    w = rt_w;
    v.posScale[0] = 2.0f / rt_w;
    v.posOffset[0] = (xo_en ? xo : 0.0f) * 2.0f / rt_w - 1.0f;
  }
  if (ys_en && ys != 0.0f) {
    h = 2.0f * std::fabs(ys);
    y0 = (yo_en ? yo : 0.0f) - std::fabs(ys);
    v.posScale[1] = ys < 0 ? 1.0f : -1.0f;
  } else {
    y0 = 0.0f;
    h = rt_h;
    v.posScale[1] = -2.0f / rt_h;
    v.posOffset[1] = 1.0f - (yo_en ? yo : 0.0f) * 2.0f / rt_h;
  }
  float zmin, zmax;
  if (zs_en) {
    if (zs >= 0.0f) {
      zmin = zo_en ? zo : 0.0f;
      zmax = zmin + zs;
    } else {
      zmax = zo_en ? zo : 0.0f;
      zmin = zmax + zs;
      v.posScale[2] = -1.0f;
      v.posOffset[2] = 1.0f;
    }
  } else {
    zmin = 0.0f;
    zmax = 1.0f;
    v.posOffset[2] = zo_en ? zo : 0.0f;
  }
  zmin = std::clamp(zmin, 0.0f, 1.0f);
  zmax = std::clamp(zmax, 0.0f, 1.0f);
  if (dev.U32(dev::kModeControl) & (1u << 11)) {
    const bool scene = t.colorCount > 0 || (t.width == kGuestRenderWidth && t.height == kGuestRenderHeight);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    if (scene && offset != 0.0f && std::isfinite(offset) && zmax > zmin)
      v.posOffset[2] += offset / (zmax - zmin);
  }
  if ((dev.U32(dev::kVtxControl) & 1) == 0) {
    v.posOffset[0] += 1.0f / std::max(1.0f, w);
    v.posOffset[1] -= 1.0f / std::max(1.0f, h);
  }
  if ((jitter_x != 0.0f || jitter_y != 0.0f) && xs_en && ys_en && t.width == kGuestRenderWidth &&
      t.height == kGuestRenderHeight) {
    v.posOffset[0] += jitter_x * 2.0f / std::max(1.0f, w * t.scale);
    v.posOffset[1] -= jitter_y * 2.0f / std::max(1.0f, h * t.scale);
  }
  const float S = t.scale;
  const float hx0 = std::round(x0 * S), hx1 = std::round((x0 + w) * S);
  const float hy0 = std::round(y0 * S), hy1 = std::round((y0 + h) * S);
  v.vp = plume::RenderViewport(hx0, hy0, std::max(hx1 - hx0, 1.0f), std::max(hy1 - hy0, 1.0f),
                               zmin, zmax);

  auto scissor_of = [](u32 tl, u32 br) {
    return plume::RenderRect(static_cast<i32>(tl & 0x7FFF), static_cast<i32>((tl >> 16) & 0x7FFF),
                             static_cast<i32>(br & 0x7FFF), static_cast<i32>((br >> 16) & 0x7FFF));
  };
  const plume::RenderRect win =
      scissor_of(dev.U32(dev::kWindowScissorTL), dev.U32(dev::kWindowScissorBR));
  const plume::RenderRect scr =
      scissor_of(dev.U32(dev::kScreenScissorTL), dev.U32(dev::kScreenScissorBR));
  plume::RenderRect sc;
  sc.left = std::max({0, win.left, scr.left});
  sc.top = std::max({0, win.top, scr.top});
  sc.right = std::min({static_cast<i32>(t.width), win.right, scr.right});
  sc.bottom = std::min({static_cast<i32>(t.height), win.bottom, scr.bottom});
  if (sc.right <= sc.left || sc.bottom <= sc.top)
    sc = plume::RenderRect(0, 0, static_cast<i32>(t.width), static_cast<i32>(t.height));
  sc = plume::RenderRect(ScalePxBy(sc.left, S), ScalePxBy(sc.top, S),
                         std::max(ScalePxBy(sc.right, S), ScalePxBy(sc.left, S) + 1),
                         std::max(ScalePxBy(sc.bottom, S), ScalePxBy(sc.top, S) + 1));
  if (t.offsetX || t.offsetY) {
    v.vp.x += static_cast<float>(t.offsetX);
    v.vp.y += static_cast<float>(t.offsetY);
    sc.left += t.offsetX;
    sc.right += t.offsetX;
    sc.top += t.offsetY;
    sc.bottom += t.offsetY;
  }
  v.scissor = sc;
  return v;
}

struct ConstFileCache {
  alignas(16) u8 bytes[256 * 16] = {};
  UploadAlloc alloc;
  u64 epoch = ~0ull;
  u32 regs = 0;
};

bool UploadFloatFile(VideoState &s, DeviceView dev, u32 offset, u32 stage, u32 regs,
                     u64 dirty, UploadAlloc *out) {
  static ConstFileCache caches[2];
  static const bool verify = Settings::FastSettersVerify();
  regs = std::clamp(regs, 16u, 256u);
  const u32 bytes = regs * 16;
  const u8 *src = dev.Bytes(offset, bytes);
  if (!src)
    return false;
  ConstFileCache &c = caches[stage & 1];
  const u32 blocks = (bytes + 63) / 64;
  const u64 in_file = blocks >= 64 ? ~0ull : ((1ull << blocks) - 1);
  auto compare_blocks = [&](u64 mask) {
    u64 changed = 0;
    for (u64 m = mask & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      const u32 n = std::min(64u, bytes - 64 * b);
      if (std::memcmp(c.bytes + 64 * b, src + 64 * b, n) != 0)
        changed |= 1ull << b;
    }
    return changed;
  };
  u64 refresh = ~0ull;
  if (c.epoch == UploadRingEpoch() && c.regs >= regs) {
    if (dirty == 0) {
      *out = c.alloc;
      s.perf.const_file_hits++;
      s.perf.const_file_clean_hits++;
      return true;
    }
    u64 changed = dirty == ~0ull ? (std::memcmp(c.bytes, src, bytes) != 0 ? ~0ull : 0ull)
                                 : compare_blocks(dirty);
    if (verify) {
      const u64 actual = compare_blocks(~0ull);
      if (actual & ~dirty) {
        static u32 logged = 0;
        if (logged++ < 24)
          EOT_WARN("[draw] float file {} changed outside its pending mask: mask {:#018x} changed {:#018x} "
                   "(regs {})",
                   stage, dirty, actual, regs);
        s.perf.const_file_mask_misses++;
        changed |= actual;
      }
    }
    if (!changed) {
      *out = c.alloc;
      c.regs = regs;
      s.perf.const_file_hits++;
      return true;
    }
    refresh = changed;
  }
  if (!UploadAllocate(bytes, kConstantBufferAlignment, out))
    return false;
  rex::memory::copy_and_swap_32_unaligned(out->cpu, reinterpret_cast<const u32 *>(src), regs * 4);
  if (refresh == ~0ull) {
    std::memcpy(c.bytes, src, bytes);
  } else {
    for (u64 m = refresh & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      std::memcpy(c.bytes + 64 * b, src + 64 * b, std::min(64u, bytes - 64 * b));
    }
  }
  c.alloc = *out;
  c.epoch = UploadRingEpoch();
  c.regs = regs;
  s.perf.constant_bytes += bytes;
  return true;
}

void FillLoopConstants(DeviceView dev, u32 offset, i32 (*dst)[4]) {
  for (u32 i = 0; i < 16; ++i) {
    const u32 v = dev.U32(offset + 4 * i);
    dst[i][0] = static_cast<i32>(v & 0xFF);
    dst[i][1] = static_cast<i32>((v >> 8) & 0xFF);
    dst[i][2] = static_cast<i32>(static_cast<i8>((v >> 16) & 0xFF));
    dst[i][3] = 0;
  }
}

struct GeometryPlan {
  const CachedIndexRange *cached = nullptr;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  bool indexed = false;
  std::vector<u32> indices;
  u32 vertexCount = 0;
  u32 startVertex = 0;
  u32 minVertex = 0;
  u32 maxVertex = 0;
  i32 baseVertex = 0;
  bool rectList = false;
  u32 stream0OverrideVa = 0;
  u32 stream0OverrideStride = 0;
};

bool ReadGuestIndices(u32 ib_va, u32 start_index, u32 count, std::vector<u32> &out) {
  if (!ib_va || !count)
    return false;
  const u32 common = mem::load<u32>(ib_va + obj::kCommon);
  const u32 base = mem::load<u32>(ib_va + obj::kBufferFetch0);
  const u32 size = mem::load<u32>(ib_va + obj::kBufferFetch1);
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  if (!base)
    return false;
  if (size && (u64(start_index) + count) * elem > size) {
    u32 n;
    if (DiagShouldLog(0x6100, &n))
      EOT_WARN("[draw] index range {}+{} exceeds buffer {:#x} size {}", start_index, count, ib_va,
               size);
    count = static_cast<u32>(size / elem) > start_index ? size / elem - start_index : 0;
    if (!count)
      return false;
  }
  out.resize(count);
  if (is32) {
    auto *p = mem::at<be_u32>(base + start_index * 4);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  } else {
    auto *p = mem::at<be_u16>(base + start_index * 2);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  }
  return true;
}

void ExpandIndices(u32 prim, std::vector<u32> &idx, bool has_restart) {
  std::vector<u32> out;
  switch (prim) {
  case kPrimTriangleStrip: {
    out.reserve(idx.size() * 3);
    u32 run = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i] == 0xFFFFFFFFu)) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= 3) {
        const u32 a = idx[i - 2], b = idx[i - 1], c = idx[i];
        if ((run & 1) == 1) {
          out.push_back(a);
          out.push_back(b);
          out.push_back(c);
        } else {
          out.push_back(b);
          out.push_back(a);
          out.push_back(c);
        }
      }
    }
    break;
  }
  case kPrimTriangleFan: {
    out.reserve(idx.size() * 3);
    for (size_t i = 2; i < idx.size(); ++i) {
      out.push_back(idx[0]);
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  case kPrimQuadList: {
    out.reserve(idx.size() / 4 * 6);
    for (size_t i = 0; i + 3 < idx.size(); i += 4) {
      out.push_back(idx[i]);
      out.push_back(idx[i + 1]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i + 3]);
    }
    break;
  }
  case kPrimLineStrip: {
    out.reserve(idx.size() * 2);
    for (size_t i = 1; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i - 1] == 0xFFFF))
        continue;
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  default:
    return;
  }
  idx.swap(out);
}

struct StreamInfo {
  u32 stream = 0;
  u32 stride = 0;
  const u8 *data = nullptr;
  u32 sizeBytes = 0;
  u32 dataVa = 0;
  u32 objectVa = 0;
};

bool ReadStream(DeviceView dev, u32 stream, StreamInfo &out) {
  out.stream = stream;
  out.stride = dev.U8(dev::kStreamStride0 + stream) * 4u;
  const u32 slot = dev::StreamFetchSlotOffset(stream);
  const u32 d0 = dev.U32(slot);
  const u32 d1 = dev.U32(slot + 4);
  if ((d0 & 3) != 3 || !out.stride)
    return false;
  out.data = mem::phys<u8>(d0 & ~3u);
  out.sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4u;
  out.dataVa = d0 & ~3u;
  out.objectVa = dev.U32(dev::kStreamObject0 + 4 * stream);
  return out.data != nullptr;
}

void CopyVertexBytes(u8 *dst, const u8 *src, u32 bytes) {
  const u32 dwords = bytes / 4;
  rex::memory::copy_and_swap_32_unaligned(dst, src, dwords);
  if (bytes & 3)
    std::memcpy(dst + dwords * 4, src + dwords * 4, bytes & 3);
}

struct IndexCacheKey {
  u32 ib_va = 0, data_va = 0, start = 0, count = 0, prim = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const IndexCacheKey &o) const {
    return ib_va == o.ib_va && data_va == o.data_va && start == o.start && count == o.count &&
           prim == o.prim && seq == o.seq && sample == o.sample;
  }
};
struct IndexCacheKeyHash {
  size_t operator()(const IndexCacheKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix(k.ib_va);
    mix(k.data_va);
    mix((u64(k.start) << 32) | k.count);
    mix(k.prim);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct IndexCacheChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  std::unique_ptr<plume::RenderBuffer> vram;
  u64 vramValidUpTo = 0;
  u64 sealed = 0;
  u8 *cpu = nullptr;
  u64 capacity = 0, used = 0;
  u64 accounted = 0;
  u64 lastUseFrame = 0;
};
struct IndexCache {
  std::unordered_map<IndexCacheKey, CachedIndexRange, IndexCacheKeyHash> map;
};
IndexCache &index_cache() {
  static IndexCache c;
  return c;
}
constexpr u64 kIndexCacheChunkBytes = 8ull << 20;
constexpr u64 kIndexCacheBudgetBytes = 96ull << 20;

u64 SampleHostBytes(const u8 *p, u32 bytes) {
  if (!p || bytes < 4)
    return 0;
  u64 h = 1469598103934665603ull;
  auto mix = [&](u32 off) {
    u32 v;
    std::memcpy(&v, p + off, 4);
    h ^= v;
    h *= 1099511628211ull;
  };
  const u32 span = bytes >= 16 ? bytes - 16 : 0;
  for (u32 k = 0; k < 2; ++k) {
    const u32 base = static_cast<u32>((u64(span) * k) & ~3ull);
    for (u32 i = 0; i < 16 && base + i + 4 <= bytes; i += 4)
      mix(base + i);
  }
  return h;
}
u64 SampleGuestBytes(u32 va, u32 bytes) { return SampleHostBytes(mem::at<u8>(va), bytes); }

void PrefetchSampleProbes(const u8 *p, u64 bytes) {
  if (!p || bytes < 4)
    return;
  eot::cpu::Prefetch(p);
  eot::cpu::Prefetch(p + 15);
  if (bytes >= 16) {
    const u64 span = (bytes - 16) & ~3ull;
    eot::cpu::Prefetch(p + span);
    eot::cpu::Prefetch(p + span + 15);
  }
}

}

void PrefetchIndexProbes(u32 device_va, u32 start_index, u32 index_count) {
  if (!device_va || !index_count)
    return;
  const u8 *dev = mem::at<u8>(device_va);
  if (!dev)
    return;
  const u32 ib_va = rex::memory::load_and_swap<u32>(dev + dev::kIndexBuffer);
  const u8 *ib = ib_va ? mem::at<u8>(ib_va) : nullptr;
  if (!ib)
    return;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return;
  const u32 elem = (common & obj::kIndexBuffer32BitBit) ? 4 : 2;
  const u64 start = u64(start_index) * elem;
  u64 bytes = u64(index_count) * elem;
  if (size && start + bytes > size)
    bytes = start < size ? size - start : 0;
  if (start > 0xFFFFFFFFu - data_va)
    return;
  PrefetchSampleProbes(mem::at<u8>(data_va + static_cast<u32>(start)), bytes);
}

u64 DrawSortKey(const DrawPacket &pk, u32 *depth_func) {
  if (pk.rectList || !pk.vs)
    return 0;
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  const u32 dc = dev.U32(dev::kDepthControl);
  if (!(dc & 2))
    return 0;
  if (dc & 1) {
    auto constant_write = [](u32 func, u32 fail, u32 zpass, u32 zfail) {
      auto ok = [](u32 op) { return op == 0 || op == 1 || op == 2; };
      return func == 7 && ok(fail) && ok(zpass) && ok(zfail);
    };
    if (!constant_write((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7))
      return 0;
    if ((dc & 0x80) &&
        !constant_write((dc >> 20) & 7, (dc >> 23) & 7, (dc >> 26) & 7, (dc >> 29) & 7))
      return 0;
  }
  const u32 zfunc = (dc >> 4) & 7;
  const bool ordered = zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6;
  if (!(dc & 4) || !ordered)
    return 0;
  if (depth_func)
    *depth_func = zfunc;
  const u32 cc = dev.U32(dev::kColorControl), bc = dev.U32(dev::kBlendControl0);
  const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
  const bool passthrough = src == 1 && dst == 0 && op == 0;
  if (!(cc & (1u << 5)) && !passthrough)
    return 0;
  u64 h = 0x9E3779B97F4A7C15ull;
  auto mix = [&](u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
  };
  mix(reinterpret_cast<u64>(pk.vs));
  mix(reinterpret_cast<u64>(pk.ps));
  mix(reinterpret_cast<u64>(pk.layout));
  mix(static_cast<u64>(pk.topology));
  for (u32 S = 0; S <= pk.max_slot && S < 16; ++S)
    mix(pk.strides[S]);
  constexpr u32 kRegs[] = {dev::kModeControl,   dev::kDepthControl, dev::kBlendControl0,
                           dev::kBlendControl1, dev::kBlendControl2, dev::kBlendControl3,
                           dev::kColorControl,  dev::kColorMask,     dev::kAlphaRef};
  for (u32 off : kRegs)
    mix(dev.U32(off));
  mix(dev.U32(dev::kStencilRefMask));
  return h ? h : 1;
}

namespace {

struct BufferPool {
  std::mutex mutex;
  std::vector<IndexCacheChunk> chunks;
  u64 totalBytes = 0;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  std::atomic<bool> hasRetired{false};
  std::atomic<u64> flushEpoch{1};
};
void PoolDrainRetired(VideoState &s, BufferPool &pool) {
  if (!pool.hasRetired.load(std::memory_order_acquire))
    return;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  {
    std::lock_guard lock(pool.mutex);
    pool.hasRetired.store(false, std::memory_order_relaxed);
    if (pool.retired.empty())
      return;
    retired.swap(pool.retired);
  }
  for (auto &b : retired) {
    if (!b)
      continue;
    if (RenderThreadActive()) {
      RenderThreadRetire(b.release());
    } else {
      std::lock_guard video_lock(s.mutex);
      ParkBuffer(s, std::move(b));
    }
  }
}
bool PoolAllocate(VideoState &s, BufferPool &pool, u64 budget, u64 chunk_bytes,
                  plume::RenderBufferFlags flags, const char *name, u64 bytes, u64 align,
                  plume::RenderBuffer **buffer, u64 *offset, u8 **cpu, bool *reset) {
  *reset = false;
  if (pool.totalBytes + bytes > budget) {
    for (auto &ch : pool.chunks) {
      pool.retired.push_back(std::move(ch.buffer));
      if (ch.vram)
        pool.retired.push_back(std::move(ch.vram));
    }
    pool.hasRetired.store(true, std::memory_order_release);
    pool.chunks.clear();
    pool.totalBytes = 0;
    *reset = true;
    EOT_DEBUG("[draw] {} pool over budget ({} MB); rebuilt", name, budget >> 20);
  }
  auto align_up = [align](u64 v) { return (v + align - 1) / align * align; };
  if (pool.chunks.empty() ||
      align_up(pool.chunks.back().used) + bytes > pool.chunks.back().capacity) {
    IndexCacheChunk ch;
    const u64 size = std::max(chunk_bytes, bytes + align);
    plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
    desc.flags = flags;
    ch.buffer = CreateHostBuffer(s.device.get(), desc, name);
    if (!ch.buffer)
      return false;
    ch.cpu = static_cast<u8 *>(ch.buffer->map());
    if (!ch.cpu) {
      ch.buffer.reset();
      return false;
    }
    ch.capacity = size;
    {
      plume::RenderBufferDesc vram_desc = plume::RenderBufferDesc::DefaultBuffer(size, flags);
      const std::string vram_name = std::string(name) + "-vram";
      ch.vram = CreateHostBuffer(s.device.get(), vram_desc, vram_name.c_str());
    }
    pool.chunks.push_back(std::move(ch));
  }
  auto &ch = pool.chunks.back();
  const u64 off = align_up(ch.used);
  *buffer = ch.buffer.get();
  *offset = off;
  *cpu = ch.cpu + off;
  ch.used = off + bytes;
  ch.accounted += bytes;
  pool.totalBytes += bytes;
  return true;
}

BufferPool &index_pool() {
  static BufferPool p;
  return p;
}

void PoolSeal(BufferPool &pool, plume::RenderBuffer *buffer, u64 end) {
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (ch.buffer.get() != buffer)
      continue;
    if (end > ch.sealed)
      ch.sealed = end;
    return;
  }
}

plume::RenderBuffer *PoolResidentBuffer(VideoState &s, BufferPool &pool,
                                        plume::RenderBuffer *upload, u64 offset, u64 bytes,
                                        u64 *capacity = nullptr) {
  std::lock_guard lock(pool.mutex);
  for (const auto &ch : pool.chunks) {
    if (ch.buffer.get() != upload)
      continue;
    if (capacity)
      *capacity = ch.capacity;
    if (ch.vram && offset + bytes <= ch.vramValidUpTo) {
      s.perf.geometry_vram_binds++;
      return ch.vram.get();
    }
    break;
  }
  s.perf.geometry_staging_binds++;
  return upload;
}

void PoolFlushToVram(VideoState &s, BufferPool &pool, plume::RenderBufferFlags flags) {
  auto *cmd = s.command_list;
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (!ch.vram || ch.sealed <= ch.vramValidUpTo)
      continue;
    const u64 from = ch.vramValidUpTo, bytes = ch.sealed - from;
    const plume::RenderBufferBarrier to_copy(ch.vram.get(), plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1, nullptr, 0);
    cmd->copyBufferRegion(plume::RenderBufferReference(ch.vram.get(), from),
                          plume::RenderBufferReference(ch.buffer.get(), from), bytes);
    const plume::RenderBufferBarrier to_read(ch.vram.get(), plume::RenderBufferAccess::READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1, nullptr, 0);
    (void)flags;
    ch.vramValidUpTo = ch.sealed;
    pool.flushEpoch.fetch_add(1, std::memory_order_release);
  }
}
bool IndexCacheAllocate(VideoState &s, u64 bytes, plume::RenderBuffer **buffer, u64 *offset,
                        u8 **cpu) {
  auto &pool = index_pool();
  auto &cache = index_cache();
  if (bytes > kIndexCacheBudgetBytes)
    return false;
  std::lock_guard lock(pool.mutex);

  auto capacity = [&] {
    u64 c = 0;
    for (const auto &ch : pool.chunks)
      c += ch.capacity;
    return c;
  };
  auto needs_chunk = [&] {
    return pool.chunks.empty() || ((pool.chunks.back().used + 3) & ~3ull) + bytes > pool.chunks.back().capacity;
  };
  auto over_budget = [&] {
    return pool.totalBytes + bytes > kIndexCacheBudgetBytes ||
           (needs_chunk() && capacity() + std::max(kIndexCacheChunkBytes, bytes) > kIndexCacheBudgetBytes);
  };
  bool recycled_one = false;
  while (over_budget() && !pool.chunks.empty()) {
    auto victim = std::min_element(
        pool.chunks.begin(), pool.chunks.end(),
        [](const IndexCacheChunk &a, const IndexCacheChunk &b) {
          return a.lastUseFrame < b.lastUseFrame;
        });
    plume::RenderBuffer *dead = victim->buffer.get();
    u32 erased = 0;
    for (auto it = cache.map.begin(); it != cache.map.end();) {
      if (it->second.buffer != dead) {
        ++it;
        continue;
      }
      it = cache.map.erase(it);
      ++erased;
    }
    const u64 released = victim->accounted;
    const bool gpu_idle = victim->lastUseFrame + kNumFrames < s.guest_frames;
    IndexCacheChunk recycled;
    if (gpu_idle && bytes <= victim->capacity && !recycled_one) {
      recycled_one = true;
      recycled = std::move(*victim);
      recycled.used = 0;
      recycled.sealed = 0;
      recycled.accounted = 0;
      recycled.vramValidUpTo = 0;
      recycled.lastUseFrame = s.guest_frames;
    } else {
      pool.retired.push_back(std::move(victim->buffer));
      if (victim->vram)
        pool.retired.push_back(std::move(victim->vram));
      pool.hasRetired.store(true, std::memory_order_release);
    }
    pool.chunks.erase(victim);
    pool.totalBytes -= std::min(pool.totalBytes, released);
    if (recycled.buffer)
      pool.chunks.push_back(std::move(recycled));
    s.perf.index_cache_evictions++;
    u32 n;
    if (DiagShouldLog(0x5A11, &n))
      EOT_DEBUG("[draw] index-cache recycled one chunk ({} ranges, {} MB resident)", erased,
                pool.totalBytes >> 20);
  }
  bool reset = false;
  const bool ok = PoolAllocate(s, pool, kIndexCacheBudgetBytes, kIndexCacheChunkBytes,
                               plume::RenderBufferFlag::INDEX, "index-cache", bytes, 4, buffer,
                               offset, cpu, &reset);
  if (reset)
    cache.map.clear();
  if (ok) {
    for (auto &chunk : pool.chunks) {
      if (chunk.buffer.get() == *buffer) {
        chunk.lastUseFrame = s.guest_frames;
        break;
      }
    }
  }
  return ok;
}

void TouchIndexCacheBuffer(plume::RenderBuffer *buffer, u64 frame) {
  auto &pool = index_pool();
  std::lock_guard lock(pool.mutex);
  for (auto &chunk : pool.chunks) {
    if (chunk.buffer.get() == buffer) {
      chunk.lastUseFrame = frame;
      return;
    }
  }
}

struct VertexMirrorKey {
  u32 data_va = 0, size = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const VertexMirrorKey &o) const {
    return data_va == o.data_va && size == o.size && seq == o.seq && sample == o.sample;
  }
};
struct VertexMirrorKeyHash {
  size_t operator()(const VertexMirrorKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix((u64(k.data_va) << 32) | k.size);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct VertexMirror {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 size = 0;
  u64 lastUseFrame = 0;
  mutable plume::RenderBuffer *resident = nullptr;
  mutable u64 residentCapacity = 0;
  mutable u64 residentEpoch = 0;
};
struct VertexMirrorCache {
  std::unordered_map<VertexMirrorKey, VertexMirror, VertexMirrorKeyHash> map;
  std::unordered_map<VertexMirrorKey, u64, VertexMirrorKeyHash> seen;
  BufferPool pool;
  bool admissionClosed = false;
};
VertexMirrorCache &vertex_mirrors() {
  static VertexMirrorCache c;
  return c;
}
constexpr u64 kVertexMirrorChunkBytes = 16ull << 20;
constexpr u64 kVertexMirrorBudgetBytes = 256ull << 20;
constexpr u32 kVertexMirrorMaxBytes = 32u << 20;
constexpr size_t kVertexMirrorSeenCap = 4096;

const VertexMirror *GetVertexMirror(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  if (!st.dataVa || !bytes || bytes > kVertexMirrorMaxBytes || !st.data ||
      first > UINT32_MAX - st.dataVa)
    return nullptr;
  auto &c = vertex_mirrors();
  VertexMirrorKey key;
  key.data_va = st.dataVa + static_cast<u32>(first);
  key.size = static_cast<u32>(bytes);
  key.seq = st.objectVa ? ResourceUnlockSeq(st.objectVa) : 0;
  key.sample = SampleHostBytes(st.data + first, key.size);
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    it->second.lastUseFrame = s.guest_frames;
    s.perf.vertex_cache_hits++;
    return &it->second;
  }
  if (c.admissionClosed)
    return nullptr;
  auto seen = c.seen.find(key);
  if (seen == c.seen.end()) {
    if (c.seen.size() >= kVertexMirrorSeenCap)
      c.seen.clear();
    c.seen.emplace(key, s.guest_frames);
    return nullptr;
  }
  if (seen->second >= s.guest_frames)
    return nullptr;
  c.seen.erase(seen);
  if (c.pool.totalBytes > kVertexMirrorBudgetBytes ||
      bytes > kVertexMirrorBudgetBytes - c.pool.totalBytes) {
    c.admissionClosed = true;
    EOT_DEBUG("[draw] vertex-mirror cache full ({} MB, {} ranges); keeping residents and "
              "using frame uploads for new ranges",
              c.pool.totalBytes >> 20, c.map.size());
    return nullptr;
  }
  s.perf.vertex_cache_misses++;
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  bool reset = false;
  bool ok = false;
  {
    std::lock_guard lock(c.pool.mutex);
    ok = PoolAllocate(s, c.pool, kVertexMirrorBudgetBytes, kVertexMirrorChunkBytes,
                      plume::RenderBufferFlag::VERTEX, "vertex-mirror", bytes,
                      std::max<u64>(4, st.stride), &buffer, &offset, &cpu, &reset);
  }
  if (reset)
    c.map.clear();
  if (!ok)
    return nullptr;
  {
    PerfScope copy_scope(s.perf.vertex_copy_ms);
    CopyVertexBytes(cpu, st.data + first, static_cast<u32>(bytes));
  }
  PoolSeal(c.pool, buffer, offset + bytes);
  s.perf.vertex_bytes += bytes;
  VertexMirror m;
  m.buffer = buffer;
  m.offset = offset;
  m.size = static_cast<u32>(bytes);
  m.lastUseFrame = s.guest_frames;
  return &c.map.emplace(key, m).first->second;
}

void NoteMorphStream(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  struct Seen {
    u64 sample = 0, full = 0;
  };
  static std::unordered_map<u64, Seen> seen;
  static u64 frames = 0, last_frame = ~0ull, draws = 0, changed = 0, stale = 0;
  if (!st.data || !bytes || bytes > kVertexMirrorMaxBytes)
    return;
  const u64 key = (u64(st.dataVa + static_cast<u32>(first)) << 32) ^ bytes;
  const u64 sample = SampleHostBytes(st.data + first, static_cast<u32>(bytes));
  const u64 full = XXH3_64bits(st.data + first, static_cast<size_t>(bytes));
  auto [it, inserted] = seen.emplace(key, Seen{sample, full});
  if (!inserted) {
    if (it->second.full != full) {
      changed++;
      if (it->second.sample == sample)
        stale++;
    }
    it->second = Seen{sample, full};
  }
  draws++;
  if (last_frame != s.guest_frames) {
    last_frame = s.guest_frames;
    if (++frames % 600 == 0) {
      EOT_DEBUG("[morph] {} draws over {} frames: {} ranges, {} rewrites, {} of them under an "
                "unchanged fingerprint",
                draws, frames, seen.size(), changed, stale);
      if (seen.size() > 4096)
        seen.clear();
    }
  }
}

const CachedIndexRange *GetCachedIndexRange(VideoState &s, u32 ib_va, u32 prim, u32 start_index,
                                            u32 count) {
  if (!ib_va || !count || prim == kPrimRectList)
    return nullptr;
  const u8 *ib = mem::at<u8>(ib_va);
  if (!ib)
    return nullptr;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return nullptr;
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  const u64 range_start = u64(start_index) * elem;
  u64 range_bytes = u64(count) * elem;
  if (size && range_start + range_bytes > size)
    range_bytes = range_start < size ? size - range_start : 0;
  if (!range_bytes)
    return nullptr;
  IndexCacheKey key;
  key.ib_va = ib_va;
  key.data_va = data_va;
  key.start = start_index;
  key.count = count;
  key.prim = prim;
  key.seq = ResourceUnlockSeq(ib_va);
  key.sample = SampleGuestBytes(data_va + static_cast<u32>(range_start), static_cast<u32>(range_bytes));

  auto &c = index_cache();
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    if (it->second.lastUseFrame != s.guest_frames) {
      it->second.lastUseFrame = s.guest_frames;
      TouchIndexCacheBuffer(it->second.buffer, s.guest_frames);
    }
    s.perf.index_cache_hits++;
    return &it->second;
  }
  s.perf.index_cache_misses++;

  std::vector<u32> idx;
  if (!ReadGuestIndices(ib_va, start_index, count, idx))
    return nullptr;
  bool expand = false;
  plume::RenderPrimitiveTopology topology = ConvertPrimitiveType(prim, &expand);
  if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
    ExpandIndices(prim, idx, true);
    topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                      : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  if (idx.empty())
    return nullptr;
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (u32 v : idx) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const bool host32 = hi > 0xFFFEu;
  const u64 bytes = idx.size() * (host32 ? 4 : 2);
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  const bool allocated = s.ready && IndexCacheAllocate(s, bytes, &buffer, &offset, &cpu);
  PoolDrainRetired(s, index_pool());
  if (!allocated)
    return nullptr;
  if (host32) {
    std::memcpy(cpu, idx.data(), bytes);
  } else {
    auto *dst = reinterpret_cast<u16 *>(cpu);
    for (size_t i = 0; i < idx.size(); ++i)
      dst[i] = static_cast<u16>(idx[i]);
  }
  PoolSeal(index_pool(), buffer, offset + bytes);
  CachedIndexRange r;
  r.buffer = buffer;
  r.offset = offset;
  r.count = static_cast<u32>(idx.size());
  r.lo = lo;
  r.hi = hi;
  r.is32 = host32;
  r.topology = topology;
  r.lastUseFrame = s.guest_frames;
  return &(c.map[key] = r);
}

struct RectExpansion {
  std::vector<u8> vertices[16];
  std::vector<u32> indices;
  u32 hostVertexCount = 0;
};

float LaneToFloat(const u8 *p, plume::RenderFormat f, u32 lane, bool *ok) {
  using F = plume::RenderFormat;
  *ok = true;
  switch (f) {
  case F::R32_FLOAT:
  case F::R32G32_FLOAT:
  case F::R32G32B32_FLOAT:
  case F::R32G32B32A32_FLOAT: {
    float v;
    std::memcpy(&v, p + lane * 4, 4);
    return v;
  }
  case F::R16G16_FLOAT:
  case F::R16G16B16A16_FLOAT:
  case F::R16_FLOAT: {
    u16 h;
    std::memcpy(&h, p + lane * 2, 2);
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0)
      v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
      v = man ? NAN : INFINITY;
    else
      v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
  }
  default:
    *ok = false;
    return 0.0f;
  }
}

void SynthesizeLane(u8 *dst, const u8 *vi, const u8 *vj, const u8 *vk, plume::RenderFormat f,
                    u32 bytes) {
  using F = plume::RenderFormat;
  auto lanes_of = [](F fmt, u32 *lane_bytes) -> u32 {
    switch (fmt) {
    case F::R32_FLOAT:
      *lane_bytes = 4;
      return 1;
    case F::R32G32_FLOAT:
      *lane_bytes = 4;
      return 2;
    case F::R32G32B32_FLOAT:
      *lane_bytes = 4;
      return 3;
    case F::R32G32B32A32_FLOAT:
      *lane_bytes = 4;
      return 4;
    case F::R16_FLOAT:
    case F::R16_UNORM:
    case F::R16_SNORM:
      *lane_bytes = 2;
      return 1;
    case F::R16G16_FLOAT:
    case F::R16G16_UNORM:
    case F::R16G16_SNORM:
    case F::R16G16_UINT:
      *lane_bytes = 2;
      return 2;
    case F::R16G16B16A16_FLOAT:
    case F::R16G16B16A16_UNORM:
    case F::R16G16B16A16_SNORM:
    case F::R16G16B16A16_UINT:
      *lane_bytes = 2;
      return 4;
    case F::R8G8B8A8_UNORM:
    case F::B8G8R8A8_UNORM:
    case F::R8G8B8A8_SNORM:
    case F::R8G8B8A8_UINT:
      *lane_bytes = 1;
      return 4;
    case F::R8G8_UNORM:
    case F::R8G8_SNORM:
    case F::R8G8_UINT:
      *lane_bytes = 1;
      return 2;
    case F::R8_UNORM:
    case F::R8_SNORM:
    case F::R8_UINT:
      *lane_bytes = 1;
      return 1;
    default:
      *lane_bytes = 0;
      return 0;
    }
  };
  u32 lane_bytes = 0;
  const u32 lanes = lanes_of(f, &lane_bytes);
  if (!lanes) {
    std::memcpy(dst, vi, bytes);
    return;
  }
  for (u32 l = 0; l < lanes; ++l) {
    bool ok = false;
    if (lane_bytes == 4 || (lane_bytes == 2 && (f == F::R16_FLOAT || f == F::R16G16_FLOAT ||
                                                 f == F::R16G16B16A16_FLOAT))) {
      const float a = LaneToFloat(vi, f, l, &ok), b = LaneToFloat(vj, f, l, &ok),
                  c = LaneToFloat(vk, f, l, &ok);
      const float r = a + b - c;
      if (lane_bytes == 4) {
        std::memcpy(dst + l * 4, &r, 4);
      } else {
        std::memcpy(dst + l * 2, vi + l * 2, 2);
      }
    } else if (lane_bytes == 2) {
      u16 a, b, c;
      std::memcpy(&a, vi + l * 2, 2);
      std::memcpy(&b, vj + l * 2, 2);
      std::memcpy(&c, vk + l * 2, 2);
      const i32 r = std::clamp(static_cast<i32>(a) + b - c, 0, 65535);
      const u16 rv = static_cast<u16>(r);
      std::memcpy(dst + l * 2, &rv, 2);
    } else {
      const i32 r = std::clamp(static_cast<i32>(vi[l]) + vj[l] - vk[l], 0, 255);
      dst[l] = static_cast<u8>(r);
    }
  }
}

bool ExpandRectList(const InputLayout &layout, const StreamInfo streams[16], u32 stream_mask,
                    const std::vector<u32> &guest_vertices, RectExpansion &out) {
  const plume::RenderInputElement *pos = nullptr;
  for (const auto &e : layout.elements) {
    if (std::strcmp(e.semanticName, "POSITION") == 0 && e.semanticIndex == 0 &&
        e.slotIndex != kSyntheticVertexSlot) {
      pos = &e;
      break;
    }
  }
  const u32 rects = static_cast<u32>(guest_vertices.size() / 3);
  out.hostVertexCount = rects * 4;
  out.indices.reserve(rects * 6);
  for (u32 S = 0; S < 16; ++S) {
    if (stream_mask & (1u << S))
      out.vertices[S].resize(size_t(out.hostVertexCount) * streams[S].stride);
  }
  for (u32 r = 0; r < rects; ++r) {
    const u32 g[3] = {guest_vertices[r * 3], guest_vertices[r * 3 + 1],
                      guest_vertices[r * 3 + 2]};
    u32 k = 0;
    if (pos) {
      const StreamInfo &ps = streams[pos->slotIndex];
      float px[3], py[3];
      bool ok = true;
      for (u32 n = 0; n < 3 && ok; ++n) {
        const u8 *v = ps.data + u64(g[n]) * ps.stride + pos->alignedByteOffset;
        if ((u64(g[n]) + 1) * ps.stride > ps.sizeBytes && ps.sizeBytes) {
          ok = false;
          break;
        }
        u8 tmp[16];
        CopyVertexBytes(tmp, v, 16);
        px[n] = LaneToFloat(tmp, pos->format, 0, &ok);
        py[n] = LaneToFloat(tmp, pos->format, 1, &ok);
      }
      if (ok) {
        float best = INFINITY;
        for (u32 c = 0; c < 3; ++c) {
          const u32 i = (c + 1) % 3, j = (c + 2) % 3;
          const float ax = px[i] - px[c], ay = py[i] - py[c];
          const float bx = px[j] - px[c], by = py[j] - py[c];
          const float dot = std::fabs(ax * bx + ay * by);
          const float norm = std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by)) + 1e-20f;
          const float score = dot / norm;
          if (score < best) {
            best = score;
            k = c;
          }
        }
      }
    }
    const u32 i = (k + 1) % 3, j = (k + 2) % 3;
    const u32 base = r * 4;
    for (u32 S = 0; S < 16; ++S) {
      if (!(stream_mask & (1u << S)))
        continue;
      const StreamInfo &st = streams[S];
      u8 *dst = out.vertices[S].data() + size_t(base) * st.stride;
      for (u32 n = 0; n < 3; ++n) {
        const u8 *src = st.data + u64(g[n]) * st.stride;
        CopyVertexBytes(dst + n * st.stride, src, st.stride);
      }
      u8 *v3 = dst + 3 * st.stride;
      std::memcpy(v3, dst + i * st.stride, st.stride);
      for (const auto &e : layout.elements) {
        if (e.slotIndex != S)
          continue;
        const u32 bytes = std::min(16u, st.stride - e.alignedByteOffset);
        SynthesizeLane(v3 + e.alignedByteOffset, dst + i * st.stride + e.alignedByteOffset,
                       dst + j * st.stride + e.alignedByteOffset,
                       dst + k * st.stride + e.alignedByteOffset, e.format, bytes);
      }
    }
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + i);
    out.indices.push_back(base + 3);
    out.indices.push_back(base + j);
  }
  return true;
}

void FillPipelineState(DeviceView dev, const Targets &t, PipelineState &st,
                       u32 *spec_out, bool *alpha_to_coverage_only) {
  const u32 mode = dev.U32(dev::kModeControl);
  const bool cull_front = mode & 1, cull_back = mode & 2, face_cw = mode & 4;
  if (cull_front && cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else if (cull_front)
    st.cull = plume::RenderCullMode::FRONT;
  else if (cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else
    st.cull = plume::RenderCullMode::NONE;
  if (eot::debug::FreecamDrawTwoSided())
    st.cull = plume::RenderCullMode::NONE;
  st.frontFace = face_cw ? plume::RenderFrontFace::CLOCKWISE
                         : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  const u32 clip = dev.U32(dev::kClipControl);
  st.depthClip = (clip & ((1u << 16) | (1u << 26) | (1u << 27))) == 0;
  if (!st.depthClip) {
    u32 n;
    if (DiagShouldLog(0x6C1F, &n) && n < 3)
      EOT_DEBUG("[draw] clip control {:#x}: depth clip off", clip);
  }

  if (mode & (1u << 11)) {
    const float scale = dev.F32(dev::kPolyOffsetFrontScale);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    st.slopeScaledDepthBias = scale / 16.0f;
    st.depthBias = PolygonOffsetUnits(offset);
  }
  const float rs = RenderScaleFactor();
  const float ts = std::fabs(t.scale - rs) < 0.01f ? rs : t.scale;
  st.targetScale = (st.depthBias || st.slopeScaledDepthBias != 0.0f) ? ts : 1.0f;

  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  const bool z_test = has_ds && (dc & 2);
  st.depthWrite = has_ds && (dc & 4);
  st.depthEnable = z_test || st.depthWrite;
  st.depthFunc = z_test ? ConvertCompareFunc((dc >> 4) & 7)
                        : plume::RenderComparisonFunction::ALWAYS;
  st.stencilEnable = has_ds && (dc & 1);
  if (st.stencilEnable && !FormatHasStencil(t.depthImage ? t.depthImage->format : t.depth->host.format)) {
    u32 n;
    if (DiagShouldLog(0x6C30, &n) && n == 0)
      EOT_WARN("[draw] a draw into the stencil-less depth target {:#x} ({}x{}) enables the stencil test "
               "(control {:#x}); drawn without it",
               t.depth->va, t.depth->width, t.depth->height, dc);
    st.stencilEnable = false;
  }
  if (st.stencilEnable) {
    const u32 srm = dev.U32(dev::kStencilRefMask);
    st.stencilReadMask = static_cast<u8>((srm >> 8) & 0xFF);
    st.stencilWriteMask = static_cast<u8>((srm >> 16) & 0xFF);
    st.stencilRef = static_cast<u8>(srm & 0xFF);
    st.stencilFront.compareFunction = ConvertCompareFunc((dc >> 8) & 7);
    st.stencilFront.failOp = ConvertStencilOp((dc >> 11) & 7);
    st.stencilFront.passOp = ConvertStencilOp((dc >> 14) & 7);
    st.stencilFront.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & (1u << 7)) {
      st.stencilBack.compareFunction = ConvertCompareFunc((dc >> 20) & 7);
      st.stencilBack.failOp = ConvertStencilOp((dc >> 23) & 7);
      st.stencilBack.passOp = ConvertStencilOp((dc >> 26) & 7);
      st.stencilBack.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      st.stencilBack = st.stencilFront;
    }
  } else {
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
  }

  const u32 cc = dev.U32(dev::kColorControl);
  const bool blend_disable = (cc & (1u << 5)) != 0;
  const u32 color_mask = dev.U32(dev::kColorMask);
  static const u32 kBlendRegs[4] = {dev::kBlendControl0, dev::kBlendControl1,
                                    dev::kBlendControl2, dev::kBlendControl3};
  for (u32 i = 0; i < t.colorCount; ++i) {
    const u32 bc = dev.U32(kBlendRegs[i]);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const u32 srca = (bc >> 16) & 0x1F, opa = (bc >> 21) & 7, dsta = (bc >> 24) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0 && srca == 1 && dsta == 0 && opa == 0;
    plume::RenderBlendDesc &b = st.blend[i];
    b.blendEnabled = !blend_disable && !passthrough;
    b.srcBlend = ConvertBlendFactor(src);
    b.dstBlend = ConvertBlendFactor(dst);
    b.blendOp = ConvertBlendOp(op);
    b.srcBlendAlpha = ConvertBlendFactor(srca);
    b.dstBlendAlpha = ConvertBlendFactor(dsta);
    b.blendOpAlpha = ConvertBlendOp(opa);
    b.renderTargetWriteMask = static_cast<u8>((color_mask >> (4 * i)) & 0xF);
    st.rtFormats[i] = t.colorImage[i] ? t.colorImage[i]->format : t.color[i]->host.format;
  }
  st.rtCount = t.colorCount;
  st.dsFormat = has_ds ? (t.depthImage ? t.depthImage->format : t.depth->host.format)
                       : plume::RenderFormat::UNKNOWN;
  st.sampleCount = t.samples;
  st.alphaToCoverage = (cc & (1u << 4)) != 0;
  *alpha_to_coverage_only = st.alphaToCoverage;

  u32 spec = 0;
  if (cc & (1u << 3)) {
    const u32 func = cc & 7;
    if (func == 4 || func == 6) {
      spec |= kSpecAlphaTest;
    } else if (func == 0) {
      for (u32 i = 0; i < t.colorCount; ++i)
        st.blend[i].renderTargetWriteMask = 0;
    } else if (func != 7) {
      u32 n;
      if (DiagShouldLog(0x6200 + func, &n))
        EOT_WARN("[draw] alpha test function {} not modelled (treated as pass)", func);
    }
  }
  *spec_out = spec;
}

struct SamplerBindings {
  SharedConstants shared;
};

bool RefreshResolvedMirror(VideoState &s, GuestTexture &mirror, const Targets &t) {
  if (!mirror.resolveOwned || mirror.host.isDepth || !mirror.lastResolve)
    return false;
  GuestSurface *surf = nullptr;
  for (u32 i = 0; i < t.colorCount; ++i) {
    if (t.color[i] && t.color[i]->uid == mirror.resolvedSurfaceUid) {
      surf = t.color[i];
      break;
    }
  }
  if (!surf || surf->serial == mirror.resolvedSurfaceSerial ||
      mirror.resolvedSurfaceSerial < surf->wholeClearSerial)
    return false;
  ResolvePacket pk = *mirror.lastResolve;
  pk.refresh = true;
  pk.flags &= ~0x300u;
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  ReplayResolveLocked(s, pk);
  s.defer_shader_read_transitions = defer;
  return s.command_list_open;
}

bool FlushAliasCopyForDraw(VideoState &s, GuestTexture &t) {
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  const bool copied = FlushAliasCopy(s, t);
  s.defer_shader_read_transitions = defer;
  return copied;
}

void BindTexturesAndSamplers(VideoState &s, DeviceView dev, u32 texture_mask,
                             SharedConstants &sc, const Targets *refresh_targets) {
  const u32 sampler_policy = static_cast<u32>(Settings::Anisotropy());
  const bool refresh_copies = refresh_targets && Settings::SceneCopyRefresh();
  struct DeferGuard {
    VideoState &s;
    explicit DeferGuard(VideoState &state) : s(state) { s.defer_shader_read_transitions = true; }
    ~DeferGuard() { s.defer_shader_read_transitions = false; }
  } defer_guard(s);
  for (u32 i = 0; i < 16; ++i) {
    sc.texture2DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.texture3DIndices[i] = kNullTexture3DDescriptorIndex;
    sc.textureCubeIndices[i] = kNullTextureCubeDescriptorIndex;
    sc.texture1DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.samplerIndices[i] = kSamplerLinearClamp;
  }
  const u64 generation = s.texture_generation.load(std::memory_order_relaxed);
  for (u32 slot = 0; slot < 16; ++slot)
    s.draw_bound_textures[slot] = nullptr;
  bool copied_while_binding = false;
  for (u32 slot = 0; slot < 16; ++slot) {
    if (!(texture_mask & (1u << slot)))
      continue;
    const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
    if (!tex_va)
      continue;
    const u8 *fc_raw = dev.Bytes(dev::kFetchConstants + 24 * slot, 24);
    if (!fc_raw || (fc_raw[3] & 3) != 2)
      continue;
    s.perf.texture_bind_requests++;
    GuestTexture *gt = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0;
    u8 dimension = 0, biased_bits = 0;
    VideoState::TextureSlotCache *hit = nullptr;
    for (u32 w = 0; w < VideoState::kTextureSlotWays; ++w) {
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][w];
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture && cs.texture->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *cs.texture);
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture &&
          cs.resourceGeneration == cs.texture->bindingGeneration &&
          cs.samplerPolicy == sampler_policy && std::memcmp(cs.fc, fc_raw, sizeof(cs.fc)) == 0) {
        hit = &cs;
        break;
      }
    }
    if (hit) {
      gt = hit->texture;
      gt->lastUseFrame = s.guest_frames;
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      TransitionLocked(s, gt->host, plume::RenderTextureLayout::SHADER_READ);
      index = hit->index;
      sampler = hit->sampler;
      dimension = hit->dimension;
      biased_bits = hit->biasedBits;
      s.perf.texture_bind_hits++;
    } else {
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = rex::memory::load_and_swap<u32>(fc_raw + 4 * d);
      gt = GetGuestTexture(s, tex_va);
      if (!gt) {
        u32 n;
        if (DiagShouldLog(0x6300 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} has no host mirror (fetch fmt {}); sampling null",
                   slot, tex_va, (fc[1] >> 0) & 0x3F);
        continue;
      }
      const u32 swizzle = (fc[3] >> 1) & 0xFFF;
      if (gt->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *gt);
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      index = PrepareTextureForSampling(s, *gt, swizzle);
      if (index == kInvalidDescriptorIndex) {
        u32 n;
        if (DiagShouldLog(0x6380 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} (fmt {} {}x{} {}) could not be bound; sampling null",
                   slot, tex_va, static_cast<u32>(gt->format), gt->width, gt->height,
                   gt->host.isDepth ? "depth" : "colour");
        continue;
      }
      sampler = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(
          fc, !gt->resolveOwned && !gt->host.isDepth && gt->host.mipLevels > 1, gt->synthMips));
      dimension = static_cast<u8>(gt->dimension);
      const u32 sign_x = (fc[0] >> 2) & 3;
      const u32 sign_w = (fc[0] >> 8) & 3;
      if (sign_x == 2)
        biased_bits |= 1;
      if (sign_x == 3) {
        const bool srgb_view = gt->host.format == plume::RenderFormat::BC1_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC2_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC3_UNORM_SRGB;
        if (!srgb_view)
          biased_bits |= 2;
      }
      if (sign_w == 2)
        biased_bits |= 4;
      u8 &next = s.slot_cache_next[slot];
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][next];
      next = static_cast<u8>((next + 1) % VideoState::kTextureSlotWays);
      cs.texVa = tex_va;
      std::memcpy(cs.fc, fc_raw, sizeof(cs.fc));
      cs.generation = s.texture_generation.load(std::memory_order_relaxed);
      cs.resourceGeneration = gt->bindingGeneration;
      cs.samplerPolicy = sampler_policy;
      cs.texture = gt;
      cs.index = index;
      cs.sampler = sampler;
      cs.dimension = dimension;
      cs.biasedBits = biased_bits;
    }
    gt->perfSamples++;
    s.draw_bound_textures[slot] = gt;
    switch (static_cast<xe::DataDimension>(dimension)) {
    case xe::DataDimension::k3D:
      sc.texture3DIndices[slot] = index;
      break;
    case xe::DataDimension::kCube:
      sc.textureCubeIndices[slot] = index;
      break;
    default:
      sc.texture2DIndices[slot] = index;
      sc.texture1DIndices[slot] = index;
      break;
    }
    sc.samplerIndices[slot] = sampler;
    if (biased_bits & 1)
      sc.biasedTextures |= 1u << slot;
    if (biased_bits & 2)
      sc.biasedTextures |= 1u << (16 + slot);
    if (biased_bits & 4)
      sc.sintTexcoords |= 1u << (16 + slot);
  }
  if (copied_while_binding)
    for (u32 slot = 0; slot < 16; ++slot)
      if (GuestTexture *bound = s.draw_bound_textures[slot])
        TransitionLocked(s, bound->host, plume::RenderTextureLayout::SHADER_READ);
}

bool UploadZeroBuffer(VideoState &s, UploadAlloc *out) {
  static UploadAlloc cached[kNumFrames];
  static u64 cached_frame[kNumFrames] = {};
  static bool cached_valid[kNumFrames] = {};
  const u32 slot = s.recording_slot();
  if (cached_valid[slot] && cached_frame[slot] == UploadRingEpoch()) {
    *out = cached[slot];
    return true;
  }
  if (!UploadAllocate(4096, 16, out))
    return false;
  std::memset(out->cpu, 0, 4096);
  cached[slot] = *out;
  cached_frame[slot] = UploadRingEpoch();
  cached_valid[slot] = true;
  return true;
}

static_assert(dev::kPsFloatConstants + 256u * 16u == dev::kVsBoolConstants);

bool VelocityCandidate(DeviceView dev, const TargetWords &tw) {
  if (tw.colorCount != 1 || !tw.depthVa || !tw.colorVa[0])
    return false;
  if (!(dev.U32(dev::kDepthControl) & 4))
    return false;
  if ((dev.U32(dev::kVteControl) & 5) != 5)
    return false;
  const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
  if (std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) >= 1.0f ||
      std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) >= 1.0f)
    return false;
  const u32 *w = tw.colorWords[0];
  const u32 width = (w[3] >> 18) + 1, height = ((w[3] >> 3) & 0x7FFF) + 1;
  if (width != kGuestRenderWidth || height != kGuestRenderHeight)
    return false;
  return ((w[1] >> 16) & 0xF) == 7; // ColorRenderTargetFormat k_16_16_16_16_FLOAT
}

bool ShadowTileWords(const u32 words[5]) {
  const u32 size_bits = words[3];
  return (size_bits >> 18) + 1 == 1024 && ((size_bits >> 3) & 0x7FFF) + 1 == 1024;
}

struct ShadowPassCensus {
  u64 frame = ~0ull;
  u32 pass = 0;
  u64 hash = 0;
  u32 casters = 0;
  u32 skinned = 0;
  u64 staticHash = 0;
  u64 lastHashes[64] = {};
  u64 lastStaticHashes[64] = {};
  u32 lastCasters[64] = {};
  u32 lastPasses = 0;
  u64 hashes[64] = {};
  u64 staticHashes[64] = {};
  u32 castersOf[64] = {};
  u64 sumPasses = 0, sumRepeats = 0, sumCasters = 0, sumRepeatCasters = 0, frames = 0;
  u64 sumSkinned = 0, sumStaticRepeatPasses = 0, sumStaticRepeatCasters = 0;
  u64 lastPrint = 0;
};
ShadowPassCensus g_shadow_census;

void ShadowPassClose(ShadowPassCensus &c) {
  if (!c.casters)
    return;
  if (c.pass < 64) {
    c.hashes[c.pass] = c.hash;
    c.staticHashes[c.pass] = c.staticHash;
    c.castersOf[c.pass] = c.casters;
    const bool repeat = c.skinned == 0 && c.pass < c.lastPasses && c.lastHashes[c.pass] == c.hash;
    const bool static_repeat = c.pass < c.lastPasses && c.lastStaticHashes[c.pass] == c.staticHash;
    c.sumPasses++;
    c.sumCasters += c.casters;
    c.sumSkinned += c.skinned;
    if (repeat) {
      c.sumRepeats++;
      c.sumRepeatCasters += c.casters;
    }
    if (static_repeat) {
      c.sumStaticRepeatPasses++;
      c.sumStaticRepeatCasters += c.casters - c.skinned;
    }
  }
  c.pass++;
  c.hash = 0;
  c.staticHash = 0;
  c.casters = 0;
  c.skinned = 0;
}

void NoteShadowCaster(VideoState &s, DeviceView dev, const GuestShader &vs, const StreamInfo streams[16],
                      u32 lo, u32 hi) {
  if (Settings::DiagVerbosity() < 2)
    return;
  ShadowPassCensus &c = g_shadow_census;
  if (c.frame != s.guest_frames) {
    ShadowPassClose(c);
    std::memcpy(c.lastHashes, c.hashes, sizeof(c.hashes));
    std::memcpy(c.lastStaticHashes, c.staticHashes, sizeof(c.staticHashes));
    std::memcpy(c.lastCasters, c.castersOf, sizeof(c.castersOf));
    c.lastPasses = std::min(c.pass, 64u);
    c.pass = 0;
    c.frames++;
    c.frame = s.guest_frames;
    if (s.guest_frames - c.lastPrint >= 300) {
      c.lastPrint = s.guest_frames;
      if (c.frames) {
        EOT_DEBUG("[shadow] census over {} frames: {:.1f} passes a frame, {:.1f} whole repeats of the previous "
                  "frame ({:.0f}%), {:.1f} static repeats ({:.0f}%); {:.0f} casters a frame, {:.0f} skinned, "
                  "{:.0f} in whole repeats, {:.0f} static casters in static repeats",
                  c.frames, static_cast<f64>(c.sumPasses) / c.frames,
                  static_cast<f64>(c.sumRepeats) / c.frames,
                  c.sumPasses ? 100.0 * c.sumRepeats / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumStaticRepeatPasses) / c.frames,
                  c.sumPasses ? 100.0 * c.sumStaticRepeatPasses / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumCasters) / c.frames, static_cast<f64>(c.sumSkinned) / c.frames,
                  static_cast<f64>(c.sumRepeatCasters) / c.frames,
                  static_cast<f64>(c.sumStaticRepeatCasters) / c.frames);
      }
      c.sumPasses = c.sumRepeats = c.sumCasters = c.sumRepeatCasters = c.frames = 0;
      c.sumSkinned = c.sumStaticRepeatPasses = c.sumStaticRepeatCasters = 0;
    }
  }
  u64 rows = 0x9E3779B97F4A7C15ull;
  u64 world = 0x9E3779B97F4A7C15ull;
  if (const u8 *file = dev.Bytes(dev::kVsFloatConstants, 112)) {
    for (u32 i = 16; i < 28; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      world ^= raw + 0x9E3779B97F4A7C15ull + (world << 6) + (world >> 2);
    }
    for (u32 i = 0; i < 16; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      rows ^= raw + 0x9E3779B97F4A7C15ull + (rows << 6) + (rows >> 2);
    }
  }
  static u64 pass_rows = 0;
  if (c.casters && rows != pass_rows)
    ShadowPassClose(c);
  if (!c.casters)
    pass_rows = rows;
  const bool skinned = vs.floatConstantRegs > 16;
  u64 h = c.hash ? c.hash : rows;
  const auto mix = [&h](u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
  mix(vs.hash);
  mix(streams[0].dataVa);
  mix((u64(lo) << 32) | hi);
  mix(world);
  if (skinned)
    mix(s.guest_frames);
  c.hash = h;
  if (!skinned) {
    u64 sh = c.staticHash ? c.staticHash : rows;
    sh ^= h + 0x9E3779B97F4A7C15ull + (sh << 6) + (sh >> 2);
    c.staticHash = sh;
  } else {
    c.skinned++;
  }
  c.casters++;
}

bool CaptureDraw(VideoState &s, u32 device_va, u32 prim, GeometryPlan &geom,
                 const u8 *device_image, DrawPacket &pk) {
  static u32 capture_count = 0;
  const bool timed = (++capture_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  pk.device_va = device_va;
  pk.prim = prim;
  pk.indexed = geom.indexed;
  pk.rectList = geom.rectList;
  pk.topology = geom.topology;
  pk.vertexCount = geom.vertexCount;
  pk.hasCached = false;
  pk.index_alloc = UploadAlloc{};
  pk.index_count = 0;
  pk.host_base_vertex = 0;
  pk.zero = UploadAlloc{};
  DeviceView dev = Device(device_va);
  const u8 *regs = nullptr;
  if (device_image) {
    dev.snapshot = device_image;
    dev.snapshotSize = kDeviceSnapshotBytes;
    regs = device_image;
  } else if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
    regs = live;
  }
  if (!regs) {
    Dropped("device unreadable", 0x6000);
    return false;
  }
  pk.window.Capture(regs);

  const u32 vs_va = dev.U32(dev::kVertexShader);
  const u32 ps_va = dev.U32(dev::kPixelShader);
  struct ShaderLookupCache {
    u64 generation = 0;
    u32 vs_va = 0, ps_va = 0;
    GuestShader *vs = nullptr, *ps = nullptr;
  };
  static thread_local ShaderLookupCache slc;
  const u64 shader_gen = s.shader_generation.load(std::memory_order_acquire);
  if (slc.generation != shader_gen)
    slc = ShaderLookupCache{shader_gen};
  GuestShader *vs = slc.vs_va == vs_va && slc.vs ? slc.vs : nullptr;
  if (!vs) {
    vs = FindGuestShader(s, vs_va);
    if (!vs && vs_va)
      vs = RegisterGuestShader(s, vs_va, false);
    slc.vs_va = vs_va;
    slc.vs = vs;
  }
  GuestShader *ps = slc.ps_va == ps_va && slc.ps ? slc.ps : nullptr;
  if (!ps) {
    ps = FindGuestShader(s, ps_va);
    if (!ps && ps_va)
      ps = RegisterGuestShader(s, ps_va, true);
    slc.ps_va = ps_va;
    slc.ps = ps;
  }
  if (!vs) {
    u32 n;
    if (DiagShouldLog(0x6021, &n))
      EOT_WARN("[draw] no VS: vs_va={:#x} ps_va={:#x} prim={} {} n={} rt0={:#x} decl={:#x} "
               "stream0={:#x} frame={}",
               vs_va, ps_va, prim, geom.indexed ? "idx" : "vtx", geom.vertexCount,
               dev.U32(dev::kRenderTarget0), dev.U32(dev::kVertexDeclaration),
               dev.U32(dev::kStreamObject0), s.guest_frames);
    Dropped(vs_va ? "vertex shader object unreadable" : "no vertex shader bound", 0x6001);
    return false;
  }
  if (ps_va && !ps) {
    Dropped("pixel shader object unreadable", 0x6013);
    return false;
  }
  if (!vs->entry || (ps && !ps->entry)) {
    trace::Bump(trace::Counter::ShaderMiss);
    auto note_miss = [](GuestShader &sh) {
      if (sh.entry || sh.cacheMissLogged)
        return;
      sh.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               sh.isPixel ? "ps" : "vs", sh.va, sh.hash);
    };
    note_miss(*vs);
    if (ps)
      note_miss(*ps);
    Dropped("shader cache miss", 0x6002);
    return false;
  }
  pk.vs = vs;
  pk.ps = ps;
  pk.hasCameraVP = false;
  if (Settings::Taa() || Settings::DiagVerbosity() >= 2) {
    struct CameraByTarget {
      u32 rt0 = 0;
      float vp[16] = {};
      float prevVp[16] = {};
      bool prevValid = false;
      u64 frame = ~0ull;
      bool skip = false;
    };
    static CameraByTarget cameras[4];
    static u32 camera_next = 0;
    static const CameraByTarget *camera_last = nullptr;
    static u64 capture_frame = 0;
    static u64 last_present_count = ~0ull;
    if (s.captured_presents != last_present_count) {
      last_present_count = s.captured_presents;
      ++capture_frame;
    }
    const u32 rt0 = dev.U32(dev::kRenderTarget0);
    const u32 vte = dev.U32(dev::kVteControl);
    const bool projected = (vte & 5) == 5;
    const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
    const bool frame_sized = std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) < 1.0f &&
                             std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) < 1.0f;
    if (projected && !geom.rectList && frame_sized && rt0) {
      if (const auto *m = eot::mem::at<eot::be<float>>(taa::kViewProjectionVa)) {
        CameraByTarget *slot = nullptr;
        for (CameraByTarget &c : cameras)
          if (c.rt0 == rt0)
            slot = &c;
        if (!slot)
          slot = &cameras[camera_next++ % 4];
        if (slot->rt0 != rt0) {
          slot->prevValid = false;
          slot->skip = false;
        }
        slot->rt0 = rt0;
        if (slot->frame != capture_frame) {
          if (slot->frame != ~0ull) {
            std::memcpy(slot->prevVp, slot->vp, sizeof(slot->prevVp));
            slot->prevValid = true;
          }
          slot->frame = capture_frame;
          for (u32 i = 0; i < 16; ++i)
            slot->vp[i] = m[i];
          const float motion =
              slot->prevValid
                  ? taa::CameraMotionPixels(slot->prevVp, slot->vp, static_cast<float>(InternalRenderWidth()),
                                            static_cast<float>(InternalRenderHeight()))
                  : 0.0f;
          slot->skip = taa::FastCameraGate(static_cast<u32>(slot - cameras), motion,
                                           static_cast<float>(InternalRenderWidth()));
        }
        camera_last = slot;
      }
    }
    if (camera_last && camera_last->frame == capture_frame)
      pk.taaSkip = camera_last->skip;
    if (ps && taa::IsSceneConsumer(ps->hash)) {
      const CameraByTarget *pick = nullptr;
      for (const CameraByTarget &c : cameras)
        if (c.rt0 && c.rt0 == rt0)
          pick = &c;
      if (!pick)
        pick = camera_last;
      if (pick && pick->rt0) {
        std::memcpy(pk.cameraVP, pick->vp, sizeof(pk.cameraVP));
        pk.hasCameraVP = true;
        pk.taaSkip = pick->skip;
      }
    }
  }
  pk.vs_va = vs_va;
  pk.ps_va = ps_va;

  if (!CaptureTargetWords(dev, pk.targets)) {
    Dropped("no render target or depth surface bound", 0x6003);
    return false;
  }

  const InputLayout *layout = GetInputLayout(s, *vs, dev.U32(dev::kVertexDeclaration));
  if (!layout) {
    Dropped("no input layout for the bound declaration", 0x6004);
    return false;
  }
  pk.layout = layout;
  lap(s.perf.setup_ms);
  StreamInfo streams[16];
  for (u32 S = 0; S < 16; ++S) {
    if (!(layout->streamMask & (1u << S)))
      continue;
    if (S == 0 && geom.stream0OverrideVa) {
      streams[0].stream = 0;
      streams[0].stride = geom.stream0OverrideStride;
      streams[0].data = mem::at<u8>(geom.stream0OverrideVa);
      streams[0].sizeBytes = geom.vertexCount * geom.stream0OverrideStride;
      if (!streams[0].data || !streams[0].stride) {
        Dropped("BeginVertices data unreadable", 0x6015);
        return false;
      }
      continue;
    }
    if (!ReadStream(dev, S, streams[S])) {
      Dropped("stream source unreadable", 0x6005);
      return false;
    }
    if (layout->streamExtent[S] > streams[S].stride) {
      u32 n;
      if (DiagShouldLog(0x6006 + S, &n))
        EOT_WARN("[draw] stream {} stride {} smaller than the declaration extent {}", S,
                 streams[S].stride, layout->streamExtent[S]);
    }
  }
  for (u32 S = 0; S < 16; ++S)
    pk.strides[S] = streams[S].stride;

  u32 lo = 0, hi = 0;
  RectExpansion rect;
  if (geom.rectList) {
    std::vector<u32> guest_vertices;
    if (geom.indexed) {
      guest_vertices = geom.indices;
      for (auto &v : guest_vertices)
        v = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
    } else {
      guest_vertices.resize(geom.vertexCount);
      for (u32 i = 0; i < geom.vertexCount; ++i)
        guest_vertices[i] = geom.startVertex + i;
    }
    if (!ExpandRectList(*layout, streams, layout->streamMask, guest_vertices, rect) ||
        rect.indices.empty()) {
      Dropped("rect list expansion failed", 0x6009);
      return false;
    }
    if (!UploadBytes(rect.indices.data(), rect.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(rect.indices.size());
  } else if (geom.cached) {
    pk.hasCached = true;
    pk.cached = *geom.cached;
    lo = static_cast<u32>(static_cast<i32>(geom.cached->lo) + geom.baseVertex);
    hi = static_cast<u32>(static_cast<i32>(geom.cached->hi) + geom.baseVertex);
    pk.index_count = geom.cached->count;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else if (geom.indexed) {
    if (geom.indices.empty()) {
      Dropped("empty index list", 0x600B);
      return false;
    }
    lo = 0xFFFFFFFFu;
    hi = 0;
    for (u32 v : geom.indices) {
      const u32 gv = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
      lo = std::min(lo, gv);
      hi = std::max(hi, gv);
    }
    if (!UploadBytes(geom.indices.data(), geom.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(geom.indices.size());
    s.perf.index_bytes += u64(pk.index_count) * 4;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else {
    lo = geom.startVertex;
    hi = geom.startVertex + geom.vertexCount - 1;
  }

  if (!geom.rectList) {
    for (u32 S = 0; S < 16; ++S) {
      if (!(layout->streamMask & (1u << S)) || (S == 0 && geom.stream0OverrideVa))
        continue;
      const StreamInfo &st_info = streams[S];
      const u64 first = u64(lo) * st_info.stride;
      u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
      if (st_info.sizeBytes && first + bytes > st_info.sizeBytes)
        bytes = first < st_info.sizeBytes ? st_info.sizeBytes - first : 0;
      if (bytes && bytes <= kVertexMirrorMaxBytes)
        PrefetchSampleProbes(st_info.data + first, bytes);
    }
  }

  if (!pk.targets.colorCount && pk.targets.depthVa && !ps && !geom.rectList &&
      !geom.stream0OverrideVa && ShadowTileWords(pk.targets.depthWords))
    NoteShadowCaster(s, dev, *vs, streams, lo, hi);

  pk.velocity = false;
  if (Settings::MotionVectors() && ps && !geom.rectList && vs->entry && ps->entry &&
      vs->entry->usesFloatConstants && VelocityCandidate(dev, pk.targets) &&
      VsVariantFor(vs->entry, ps->entry, false, true) == VsVariant::Velocity) {
    const StreamInfo &s0 = streams[0];
    u64 key = 0x51ED270B0E9A4C3Dull;
    auto mix = [&key](u64 v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
    mix(geom.stream0OverrideVa ? geom.stream0OverrideVa : s0.objectVa);
    mix(s0.dataVa);
    mix((u64(lo) << 32) | hi);
    mix(static_cast<u32>(geom.baseVertex));
    mix((u64(vs_va) << 32) | ps_va);
    mix(pk.indexed ? pk.index_count : pk.vertexCount);
    const u32 regs = std::clamp(vs->floatConstantRegs, 16u, 256u);
    if (const u8 *guest_file = dev.Bytes(dev::kVsFloatConstants, regs * 16)) {
      UploadAlloc block;
      if (velocity::PrepareDraw(s, key, guest_file, regs, &block)) {
        pk.vs_consts = block;
        pk.velocity = true;
      }
    }
    lap(s.perf.const_float_ms);
  }

  if ((!pk.velocity && !UploadFloatFile(s, dev, dev::kVsFloatConstants, 0, vs->floatConstantRegs,
                                        s.vs_float_constants_stale, &pk.vs_consts)) ||
      !UploadFloatFile(s, dev, dev::kPsFloatConstants, 1, ps ? ps->floatConstantRegs : 16u,
                       s.ps_float_constants_stale, &pk.ps_consts)) {
    Dropped("constant upload failed", 0x6010);
    return false;
  }
  if (!pk.velocity)
    s.vs_float_constants_stale = 0;
  s.ps_float_constants_stale = 0;
  lap(s.perf.const_float_ms);

  u32 max_slot = 0;
  for (u32 S = 0; S < 16; ++S)
    if (layout->streamMask & (1u << S))
      max_slot = S;
  pk.max_slot = max_slot;
  const u32 prefix_mask = (1u << (max_slot + 1)) - 1;
  const bool need_zero = layout->needsSyntheticSlot || layout->streamMask != prefix_mask;
  if (need_zero && !UploadZeroBuffer(s, &pk.zero)) {
    Dropped("upload ring exhausted (zero buffer)", 0x600C);
    return false;
  }
  for (u32 S = 0; S <= max_slot; ++S) {
    pk.slots[S] = plume::RenderInputSlot(S, streams[S].stride);
    if (!(layout->streamMask & (1u << S))) {
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset), 4096);
      pk.slots[S] = plume::RenderInputSlot(S, 0);
      continue;
    }
    const StreamInfo &st_info = streams[S];
    UploadAlloc va;
    if (geom.rectList) {
      if (!UploadBytes(rect.vertices[S].data(), rect.vertices[S].size(), 16, &va)) {
        Dropped("upload ring exhausted (vertices)", 0x600D);
        return false;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(va.buffer, va.offset), static_cast<u32>(va.size));
      continue;
    }
    const u64 first = u64(lo) * st_info.stride;
    u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
    if (st_info.sizeBytes && first + bytes > st_info.sizeBytes) {
      if (first >= st_info.sizeBytes) {
        Dropped("vertex range outside the stream", 0x600E);
        return false;
      }
      bytes = st_info.sizeBytes - first;
    }
    if (bytes > 64ull * 1024 * 1024) {
      Dropped("vertex range implausibly large", 0x600F);
      return false;
    }
    const bool morph = (layout->morphStreams >> S) & 1u;
    if (morph)
      NoteMorphStream(s, st_info, first, bytes);
    if (const VertexMirror *mirror = morph ? nullptr : GetVertexMirror(s, st_info, first, bytes)) {
      auto &pool = vertex_mirrors().pool;
      const u64 epoch = pool.flushEpoch.load(std::memory_order_acquire);
      if (mirror->residentEpoch != epoch || !mirror->resident) {
        mirror->resident = PoolResidentBuffer(s, pool, mirror->buffer, mirror->offset, bytes,
                                              &mirror->residentCapacity);
        mirror->residentEpoch = epoch;
      }
      if (max_slot == 0 && mirror->residentCapacity && mirror->offset % st_info.stride == 0 &&
          mirror->residentCapacity <= 0xFFFFFFFFull) {
        pk.views[S] = plume::RenderVertexBufferView(
            plume::RenderBufferReference(mirror->resident, 0),
            static_cast<u32>(mirror->residentCapacity));
        pk.host_base_vertex += static_cast<i32>(mirror->offset / st_info.stride);
        continue;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(mirror->resident, mirror->offset),
          static_cast<u32>(bytes));
      continue;
    }
    if (!UploadAllocate(bytes, 16, &va)) {
      Dropped("upload ring exhausted (vertices)", 0x600D);
      return false;
    }
    {
      PerfScope copy_scope(s.perf.vertex_copy_ms);
      CopyVertexBytes(va.cpu, st_info.data + first, static_cast<u32>(bytes));
    }
    s.perf.vertex_bytes += bytes;
    pk.views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                                static_cast<u32>(bytes));
  }
  lap(s.perf.stream_ms);

  if (geom.rectList && streams[0].data && streams[0].stride >= 8 && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    std::string verts;
    const u32 n_show = std::min<u32>(geom.vertexCount, 12u);
    for (u32 v = 0; v < n_show; ++v) {
      const u8 *p = streams[0].data + u64(v) * streams[0].stride;
      float f[4] = {0, 0, 0, 0};
      const u32 words = std::min<u32>(4u, streams[0].stride / 4u);
      for (u32 c = 0; c < words; ++c) {
        u32 raw;
        std::memcpy(&raw, p + 4 * c, 4);
        raw = (raw >> 24) | ((raw >> 8) & 0xFF00u) | ((raw << 8) & 0xFF0000u) | (raw << 24);
        std::memcpy(&f[c], &raw, 4);
      }
      verts += std::format(" [{:.3f} {:.3f} {:.3f} {:.3f}]", f[0], f[1], f[2], f[3]);
    }
    EOT_INFO("[diag]   rect verts (next draw){}", verts);
  }
  return true;
}

struct SurfaceSignature {
  const void *host = nullptr, *single = nullptr, *redirect = nullptr, *redirectHost = nullptr;
  u32 samples = 0;
  i32 redirectX = 0, redirectY = 0;
  u8 content = 0;
  bool contentInSingle = false, imagesAgree = false, resolvedSinceDraw = false;
  bool singleDirty = false, singleStale = false;
  bool operator==(const SurfaceSignature &o) const {
    return host == o.host && single == o.single && redirect == o.redirect &&
           redirectHost == o.redirectHost && samples == o.samples && redirectX == o.redirectX &&
           redirectY == o.redirectY && content == o.content &&
           contentInSingle == o.contentInSingle && imagesAgree == o.imagesAgree &&
           resolvedSinceDraw == o.resolvedSinceDraw && singleDirty == o.singleDirty &&
           singleStale == o.singleStale;
  }
};

SurfaceSignature SignSurface(const GuestSurface &surf) {
  SurfaceSignature sig{};
  sig.host = surf.host.texture.get();
  sig.single = surf.single.texture.get();
  sig.redirect = surf.redirectMirror.get();
  sig.redirectHost = surf.redirectMirror ? surf.redirectMirror->host.texture.get() : nullptr;
  sig.samples = surf.host.sampleCount;
  sig.redirectX = surf.redirectX;
  sig.redirectY = surf.redirectY;
  sig.content = static_cast<u8>(surf.content);
  sig.contentInSingle = surf.contentInSingle;
  sig.imagesAgree = surf.imagesAgree;
  sig.resolvedSinceDraw = surf.resolvedSinceDraw;
  sig.singleDirty = surf.singleDirty;
  sig.singleStale = surf.singleSerial != surf.writeSerial;
  return sig;
}

void ReplayDraw(VideoState &s, const DrawPacket &pk) {
  static u32 replay_count = 0;
  const bool timed = (++replay_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  GuestShader *vs = pk.vs;
  GuestShader *ps = pk.ps;
  const InputLayout *layout = pk.layout;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  constexpr u32 kSharedTextureBytes = offsetof(SharedConstants, booleans);
  static_assert(kSharedTextureBytes == 20 * 16);
  struct ReplayMemo {
    bool valid = false;
    GuestShader *vs = nullptr, *ps = nullptr;
    const InputLayout *layout = nullptr;
    const HostTexture *images[5] = {};
    u32 colorCount = 0, samples = 0;
    i32 offsetX = 0, offsetY = 0;
    plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    bool rectList = false;
    u32 strides[16] = {};
    u8 state[DeviceWindow::kBlocks[2].size];
    u8 consts[DeviceWindow::kBlocks[1].size];
    PipelineState st;
    u32 spec = 0;
    bool a2c = false;
    plume::RenderPipeline *pipeline = nullptr;
    ViewportInfo vp;
    u32 jitterIndex = 0;
    velocity::Target *velocity = nullptr;
    SharedConstants fixed;
    UploadAlloc shared;
    u64 sharedRing = ~0ull;
    u8 sharedTextures[kSharedTextureBytes] = {};
    u32 sharedSint = 0, sharedBiased = 0;
  };
  constexpr u32 kReplayMemoWays = 8;
  static ReplayMemo memos[kReplayMemoWays];
  static u32 memo_next = 0;
  const u8 *state_block = pk.window.image + DeviceWindow::kBlocks[2].base;
  const u8 *consts_block = pk.window.image + DeviceWindow::kBlocks[1].base;
  const u32 jitter_index = taa::JitterIndex(s, pk.taaSkip, pk.rectList);
  float jitter_x = 0.0f, jitter_y = 0.0f;
  if (!pk.rectList)
    taa::FrameJitter(s, pk.taaSkip, pk.rectList, &jitter_x, &jitter_y);
  ReplayMemo *memo = nullptr;
  for (ReplayMemo &m : memos) {
    if (m.valid && m.vs == vs && m.ps == ps && m.layout == layout && m.topology == pk.topology &&
        m.jitterIndex == jitter_index &&
        m.rectList == pk.rectList && std::memcmp(m.strides, pk.strides, sizeof(pk.strides)) == 0 &&
        std::memcmp(m.state, state_block, sizeof(m.state)) == 0 &&
        std::memcmp(m.consts, consts_block, sizeof(m.consts)) == 0) {
      memo = &m;
      break;
    }
  }
  const bool state_hit = memo != nullptr;

  struct ClsKey {
    u32 dc = 0, srm = 0, cc = 0, bc = 0;
    bool hasPs = false, rect = false;
    bool operator==(const ClsKey &o) const {
      return dc == o.dc && srm == o.srm && cc == o.cc && bc == o.bc && hasPs == o.hasPs &&
             rect == o.rect;
    }
  };
  const ClsKey cls_key{dev.U32(dev::kDepthControl), dev.U32(dev::kStencilRefMask),
                       dev.U32(dev::kColorControl), dev.U32(dev::kBlendControl0), ps != nullptr,
                       pk.rectList};
  struct TargetMemo {
    bool valid = false;
    ClsKey key;
    TargetWords words;
    u64 surfaceGeneration = 0;
    SurfaceSignature sigs[5];
    Targets targets;
    DrawClass cls;
  };
  static TargetMemo tmemo;
  Targets targets;
  DrawClass cls;
  bool target_hit = tmemo.valid && tmemo.key == cls_key &&
                    tmemo.surfaceGeneration == s.surface_generation &&
                    std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) == 0;
  if (target_hit) {
    const Targets &t = tmemo.targets;
    for (u32 i = 0; target_hit && i < t.colorCount; ++i) {
      const SurfaceSignature sig = SignSurface(*t.color[i]);
      target_hit = sig == tmemo.sigs[i];
    }
    if (target_hit && t.depth) {
      const SurfaceSignature sig = SignSurface(*t.depth);
      target_hit = sig == tmemo.sigs[4];
    }
  }
  if (target_hit) {
    targets = tmemo.targets;
    cls = tmemo.cls;
    s.perf.target_memo_hits++;
  } else {
    if (tmemo.valid) {
      if (tmemo.surfaceGeneration != s.surface_generation)
        s.perf.target_memo_miss_gen++;
      else if (!(tmemo.key == cls_key) ||
               std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) != 0)
        s.perf.target_memo_miss_words++;
      else {
        s.perf.target_memo_miss_sig++;
        static u32 logged = 0;
        if (logged < 12) {
          const Targets &t = tmemo.targets;
          for (u32 i = 0; i < 5; ++i) {
            const GuestSurface *surf = i < 4 ? (i < t.colorCount ? t.color[i] : nullptr) : t.depth;
            if (!surf)
              continue;
            const SurfaceSignature now = SignSurface(*surf), was = tmemo.sigs[i];
            if (now == was)
              continue;
            ++logged;
            EOT_DEBUG("[draw] target memo miss: surface {} ({:#x}) host {}/{} single {}/{} redirect {}/{} "
                      "samples {}/{} content {}/{} cis {}/{} agree {}/{} rsd {}/{} dirty {}/{} stale {}/{}",
                      i, surf->va, was.host, now.host, was.single, now.single, was.redirect,
                      now.redirect, was.samples, now.samples, was.content, now.content,
                      was.contentInSingle, now.contentInSingle, was.imagesAgree, now.imagesAgree,
                      was.resolvedSinceDraw, now.resolvedSinceDraw, was.singleDirty, now.singleDirty,
                      was.singleStale, now.singleStale);
          }
        }
      }
    }
    tmemo.valid = false;
    if (!ResolveTargetsFromWords(s, pk.targets, targets)) {
      Dropped("no render target or depth surface bound", 0x6003);
      return;
    }
    cls = ClassifyDraw(dev, targets, ps != nullptr, pk.rectList);
    if (cls.nullPs && !cls.depthWrite && !cls.stencil) {
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] skip prim {} n={} dc={:#x} rt0={:#x} ds={:#x} vs={:016x}: no pixel shader, no "
                 "depth or stencil write",
                 pk.prim, pk.indexed ? pk.index_count : pk.vertexCount, dev.U32(dev::kDepthControl),
                 targets.colorCount ? targets.color[0]->va : 0, targets.depth ? targets.depth->va : 0,
                 vs->hash);
      s.perf.draws--;
      s.perf.draws_skipped++;
      return;
    }
    SurfaceSignature sigs[5];
    for (u32 i = 0; i < targets.colorCount; ++i)
      sigs[i] = SignSurface(*targets.color[i]);
    if (targets.depth)
      sigs[4] = SignSurface(*targets.depth);
    if (!SelectTargetImages(s, targets, cls)) {
      Dropped("surface image unavailable", 0x6016);
      return;
    }
    tmemo.key = cls_key;
    tmemo.words = pk.targets;
    tmemo.surfaceGeneration = s.surface_generation;
    std::memcpy(tmemo.sigs, sigs, sizeof(sigs));
    tmemo.targets = targets;
    tmemo.cls = cls;
    tmemo.valid = true;
  }
  lap(s.perf.replay_targets_ms);

  if (pk.velocity && Settings::MotionVectors() && targets.colorCount == 1 && targets.depth &&
      targets.colorImage[0] && !cls.rect)
    targets.velocity = velocity::CurrentFor(s, *targets.depth, targets.colorImage[0]->width,
                                            targets.colorImage[0]->height, targets.samples);

  bool memo_hit = state_hit && memo->colorCount == targets.colorCount &&
                  memo->samples == targets.samples && memo->offsetX == targets.offsetX &&
                  memo->offsetY == targets.offsetY && memo->images[4] == targets.depthImage &&
                  memo->velocity == targets.velocity;
  for (u32 i = 0; memo_hit && i < targets.colorCount; ++i)
    memo_hit = memo->images[i] == targets.colorImage[i];

  PipelineState st;
  u32 spec = 0;
  bool a2c = false;
  plume::RenderPipeline *pipeline = nullptr;
  ViewportInfo vp;
  static SharedConstants sc;
  if (memo_hit) {
    st = memo->st;
    spec = memo->spec;
    a2c = memo->a2c;
    pipeline = memo->pipeline;
    vp = memo->vp;
    sc = memo->fixed;
    s.perf.replay_memo_hits++;
  } else {
    sc.biasedTextures = 0;
    ZeroPipelineState(st);
    spec = layout->spec;
    FillPipelineState(dev, targets, st, &spec, &a2c);
    spec |= layout->spec;
    const VsVariant variant = VsVariantFor(vs->entry, ps ? ps->entry : nullptr, ps == nullptr,
                                           targets.velocity != nullptr);
    if (targets.velocity && variant != VsVariant::Velocity)
      targets.velocity = nullptr;
    st.vs = ResolveHostShader(s, *vs, spec, variant);
    st.ps = ps ? ResolveHostShader(s, *ps, spec,
                                   variant == VsVariant::Velocity ? VsVariant::Velocity
                                                                  : VsVariant::Trimmed)
               : nullptr;
    if (targets.velocity) {
      const u32 v = st.rtCount;
      st.rtFormats[v] = velocity::kFormat;
      st.blend[v] = plume::RenderBlendDesc::Copy();
      st.rtCount = v + 1;
      st.velocity = true;
      static u64 logged_frame = ~0ull;
      if (Settings::DiagVerbosity() >= 2 && logged_frame != s.guest_frames / 600) {
        logged_frame = s.guest_frames / 600;
        EOT_DEBUG("[velocity] draw vs {:016x} ps {:016x} variant {} rt0 fmt {} blend {} mask {:#x} "
                  "rt1 fmt {} samples {} depth write {} spec {:#x}",
                  vs->hash, ps ? ps->hash : 0ull, static_cast<u32>(variant),
                  static_cast<u32>(st.rtFormats[0]), st.blend[0].blendEnabled,
                  st.blend[0].renderTargetWriteMask, static_cast<u32>(st.rtFormats[1]),
                  st.sampleCount, st.depthWrite, spec);
      }
    }
    if (!st.vs || (ps && !st.ps)) {
      Dropped("host shader unavailable (link failure)", 0x6007);
      return;
    }
    st.layout = layout;
    st.vsHash = vs->hash;
    st.psHash = ps ? ps->hash : 0;
    if (!targets.colorCount && targets.depth) {
      struct Snap {
        u32 mode, cache204, cache208, vtx, vte, win;
        float fs, fo, bs, bo, xs, xo, ys, yo;
      };
      static Snap last{};
      static u32 lines = 0;
      const Snap now{dev.U32(dev::kModeControl) & 0x1800u, mem::load<u32>(0x82496F7Cu + 204u * 4u),
                     mem::load<u32>(0x82496F7Cu + 208u * 4u), dev.U32(dev::kVtxControl),
                     dev.U32(dev::kVteControl), dev.U32(dev::kWindowOffset),
                     dev.F32(dev::kPolyOffsetFrontScale), dev.F32(dev::kPolyOffsetFrontOffset),
                     dev.F32(dev::kPolyOffsetBackScale), dev.F32(dev::kPolyOffsetBackOffset),
                     dev.F32(dev::kVportXScale), dev.F32(dev::kVportXOffset),
                     dev.F32(dev::kVportYScale), dev.F32(dev::kVportYOffset)};
      if (std::memcmp(&now, &last, sizeof(now)) != 0 && lines < 64) {
        last = now;
        ++lines;
        EOT_DEBUG("[shadow] caster draw: mode bits {:#x} front scale {:g} offset {:g} back scale {:g} "
                 "offset {:g} | cache 204 {:g} 208 {:g} -> host bias {} slope {:g} (vs {:016x}) | "
                 "vport x {:g}/{:g} y {:g}/{:g} vte {:#x} vtx {:#x} win {:#x} scale {:.4f}",
                 now.mode, now.fs, now.fo, now.bs, now.bo, std::bit_cast<float>(now.cache204),
                 std::bit_cast<float>(now.cache208), st.depthBias,
                 st.slopeScaledDepthBias * st.targetScale, st.vsHash, now.xs, now.xo, now.ys,
                 now.yo, now.vte, now.vtx, now.win, targets.scale);
      }
    }
    st.layoutKey = layout->key;
    st.spec = spec;
    for (u32 S = 0; S < 16; ++S)
      st.strides[S] = pk.strides[S];
    st.topology = pk.topology;
    if (pk.rectList)
      st.cull = plume::RenderCullMode::NONE;
    CanonicalizePipelineState(st,
                              (vs->entry ? vs->entry->specConstantsMask : 0u) |
                                  (ps && ps->entry ? ps->entry->specConstantsMask : 0u),
                              layout->streamMask);
    static PipelineState last_state{};
    static plume::RenderPipeline *last_pipeline = nullptr;
    pipeline = last_pipeline;
    if (!pipeline || std::memcmp(reinterpret_cast<const u8 *>(&st) + kPipelineKeyOffset,
                                 reinterpret_cast<const u8 *>(&last_state) + kPipelineKeyOffset,
                                 sizeof(st) - kPipelineKeyOffset) != 0) {
      pipeline = GetOrCreatePipeline(s, st);
      if (pipeline) {
        last_state = st;
        last_pipeline = pipeline;
      }
    }
    if (!pipeline) {
      Dropped("pipeline creation failed", 0x6008);
      return;
    }

    vp = ComputeViewport(dev, targets, jitter_x, jitter_y);
    for (u32 i = 0; i < 4; ++i) {
      sc.booleans[i] = dev.U32(dev::kVsBoolConstants + 4 * i);
      sc.booleans[4 + i] = dev.U32(dev::kPsBoolConstants + 4 * i);
    }
    FillLoopConstants(dev, dev::kVsLoopConstants, &sc.loopConstants[0]);
    FillLoopConstants(dev, dev::kPsLoopConstants, &sc.loopConstants[16]);
    sc.swappedTexcoords = layout->swappedTexcoords;
    sc.swappedNormals = layout->swappedNormals;
    sc.swappedBinormals = layout->swappedBinormals;
    sc.swappedTangents = layout->swappedTangents;
    sc.swappedBlendWeights = layout->swappedBlendWeights;
    sc.swappedPositions = layout->swappedPositions;
    sc.sintTexcoords = layout->sintTexcoords;
    sc.packedDec3 = layout->packedDec3;
    if (targets.colorCount) {
      const u32 f0 = targets.color[0]->colorFormat;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA))
        sc.packedDec3 |= 1u << 31;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))
        sc.packedDec3 |= 1u << 30;
    }
    sc.alphaThreshold = dev.F32(dev::kAlphaRef);
    std::memcpy(sc.posScale, vp.posScale, sizeof(sc.posScale));
    std::memcpy(sc.posOffset, vp.posOffset, sizeof(sc.posOffset));

    ReplayMemo &fill = memo ? *memo : memos[memo_next++ % kReplayMemoWays];
    fill.valid = true;
    fill.vs = vs;
    fill.ps = ps;
    fill.layout = layout;
    fill.jitterIndex = jitter_index;
    fill.velocity = targets.velocity;
    for (u32 i = 0; i < 4; ++i)
      fill.images[i] = i < targets.colorCount ? targets.colorImage[i] : nullptr;
    fill.images[4] = targets.depthImage;
    fill.colorCount = targets.colorCount;
    fill.samples = targets.samples;
    fill.offsetX = targets.offsetX;
    fill.offsetY = targets.offsetY;
    fill.topology = pk.topology;
    fill.rectList = pk.rectList;
    std::memcpy(fill.strides, pk.strides, sizeof(fill.strides));
    std::memcpy(fill.state, state_block, sizeof(fill.state));
    std::memcpy(fill.consts, consts_block, sizeof(fill.consts));
    fill.st = st;
    fill.spec = spec;
    fill.a2c = a2c;
    fill.pipeline = pipeline;
    fill.vp = vp;
    fill.fixed = sc;
    fill.sharedRing = ~0ull;
    memo = &fill;
  }
  s.current_vs_va = pk.vs_va;
  s.current_ps_va = pk.ps_va;
  {
    static const char *const kOrigins[2][3] = {{"stream/none", "stream/stream", "stream/bundle"},
                                               {"bundle/none", "bundle/stream", "bundle/bundle"}};
    s.current_origin =
        kOrigins[vs->createdByGuestCall ? 1 : 0][ps ? (ps->createdByGuestCall ? 2 : 1) : 0];
  }
  lap(s.perf.pso_lookup_ms);

  {
    PerfScope bind_scope(s.perf.bind_ms);
    const u32 texture_mask = vs->textureFetchMask | (ps ? ps->textureFetchMask : 0u);
    const u32 dc = dev.U32(dev::kDepthControl);
    const u32 zfunc = (dc >> 4) & 7;
    const bool scene_draw = !pk.rectList && (dc & 2) &&
                            (zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6);
    BindTexturesAndSamplers(s, dev, texture_mask, sc, scene_draw ? &targets : nullptr);
  }
  if (pk.hasCameraVP)
    taa::BeforeSceneConsumerDraw(s, s.draw_bound_textures, pk.cameraVP, ps ? ps->hash : 0, pk.taaSkip);
  UploadAlloc shared_alloc;
  const u64 ring_epoch = UploadRingEpoch();
  if (memo->sharedRing == ring_epoch &&
      std::memcmp(memo->sharedTextures, &sc, kSharedTextureBytes) == 0 &&
      memo->sharedSint == sc.sintTexcoords && memo->sharedBiased == sc.biasedTextures) {
    shared_alloc = memo->shared;
  } else {
    if (!UploadBytes(&sc, sizeof(sc), kConstantBufferAlignment, &shared_alloc)) {
      Dropped("shared constant upload failed", 0x6011);
      return;
    }
    s.perf.constant_bytes += sizeof(SharedConstants);
    memo->shared = shared_alloc;
    memo->sharedRing = ring_epoch;
    std::memcpy(memo->sharedTextures, &sc, kSharedTextureBytes);
    memo->sharedSint = sc.sintTexcoords;
    memo->sharedBiased = sc.biasedTextures;
  }
  lap(s.perf.const_ms);

  const bool diag_now =
      Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
  if (diag_now && Settings::Record()) {
    GpuTimingDiagMark(s, s.command_list,
                      std::format("draw rt0={:#x} {}x{} f{} smp{}{} ps={:016x} vs={:016x}",
                                  targets.colorCount ? targets.color[0]->va : 0, targets.width,
                                  targets.height,
                                  targets.colorCount ? targets.color[0]->colorFormat : 99,
                                  targets.samples,
                                  targets.depth && targets.depthImage == &targets.depth->single
                                      ? " twin"
                                      : "",
                                  ps ? ps->hash : 0, vs->hash));
  } else if (diag_now) {
    static u32 k = 0;
    const u32 vte = dev.U32(dev::kVteControl);
    GpuTimingDiagMark(s, s.command_list, std::format("draw {}", k));
    EOT_INFO("[diag] draw {} prim {} {}{} smp={}{}{}{} n={} pso={} key={:016x} mode={:#x} dc={:#x} srm={:#x} srmbf={:#x} rt0={:#x} {}x{} fmt{} rtexp{} ds={:#x} vs={:016x} ps={:016x} "
             "vp=({:.0f},{:.0f} {:.0f}x{:.0f} z{:.2f}-{:.2f}) vte={:#x} xs={:.1f} ys={:.1f} zs={:.3f} "
             "zo={:.3f} scis=({},{},{},{}) cull={} z={}{} func{} blend={} mask={:#x} spec={:#x} "
             "streams={:#x} stride0={} tex0={:#x} posScale=({:.3f},{:.3f},{:.3f}) "
             "posOff=({:.3f},{:.3f},{:.3f})",
             k++, pk.prim, pk.indexed ? "idx" : "vtx", pk.rectList ? " rect" : "", targets.samples,
             cls.additive ? " add" : "",
             targets.depth && targets.depthImage == &targets.depth->single ? " dtwin" : "",
             targets.offsetX || targets.offsetY ? " atlas" : "",
             pk.indexed ? pk.index_count : pk.vertexCount, static_cast<const void *>(pipeline),
             HashPipelineState(st), dev.U32(dev::kModeControl), dev.U32(dev::kDepthControl),
             dev.U32(dev::kStencilRefMask), dev.U32(dev::kStencilRefMaskBF),
             targets.colorCount ? targets.color[0]->va : 0, targets.width, targets.height,
             targets.colorCount ? targets.color[0]->colorFormat : 99,
             targets.colorCount ? targets.color[0]->colorExpBias : 0,
             targets.depth ? targets.depth->va : 0, vs->hash, ps ? ps->hash : 0, vp.vp.x, vp.vp.y,
             vp.vp.width, vp.vp.height, vp.vp.minDepth, vp.vp.maxDepth, vte,
             dev.F32(dev::kVportXScale), dev.F32(dev::kVportYScale), dev.F32(dev::kVportZScale),
             dev.F32(dev::kVportZOffset), vp.scissor.left, vp.scissor.top, vp.scissor.right,
             vp.scissor.bottom, static_cast<u32>(st.cull), st.depthEnable ? "on" : "off",
             st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc),
             st.blend[0].blendEnabled, st.blend[0].renderTargetWriteMask, spec,
             layout->streamMask, pk.strides[0], dev.U32(dev::kTextureObject0),
             vp.posScale[0], vp.posScale[1], vp.posScale[2], vp.posOffset[0], vp.posOffset[1],
             vp.posOffset[2]);
    for (u32 slot = 0; slot < 16 && !Settings::Record(); ++slot) {
      const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
      if (!tex_va)
        continue;
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = dev.U32(dev::kFetchConstants + 24 * slot + 4 * d);
      GuestTexture *gt = GetGuestTexture(s, tex_va);
      EOT_INFO("[diag]   tex{} va={:#x} fc=[{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] fmt={} "
               "dim={} {}x{}x{} {} {} {} biased={:#x} base={:#x} pitch={}",
               slot, tex_va, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5],
               gt ? static_cast<u32>(gt->format) : 0xFFFFu,
               gt ? static_cast<u32>(gt->dimension) : 9u, gt ? gt->width : 0u,
               gt ? gt->height : 0u, gt ? gt->depth : 0u, gt && gt->tiled ? "tiled" : "linear",
               gt && gt->gammaSigned ? "gamma" : "lin",
               gt && gt->resolveOwned ? "resolve" : "upload", sc.biasedTextures,
               gt ? gt->baseAddress : 0u, ((fc[0] >> 22) & 0x1FF) * 32);
      static u32 dumped = 0;
      if (gt && !gt->tiled && gt->format == xe::TextureFormat::k_8 && dumped < 8) {
        const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
        const u8 *src = mem::phys<const u8>(gt->baseAddress);
        if (src && pitch >= gt->width) {
          const std::string path =
              std::format("logs/f{}_tex{}_{:x}_{}x{}_p{}.bin", s.guest_frames + 1, slot, tex_va,
                          gt->width, gt->height, pitch);
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(src, 1, static_cast<size_t>(pitch) * gt->height, f);
            std::fclose(f);
            ++dumped;
            EOT_INFO("[diag]   -> wrote {}", path);
          }
        }
      }
    }
  }

  if (!BindTargets(s, targets)) {
    Dropped("framebuffer creation failed", 0x6012);
    return;
  }
  GuestSurface *work_surface = targets.colorCount ? targets.color[0] : targets.depth;
  if (work_surface)
    work_surface->perfDraws++;
  GpuTimingCountDraw(s);
  if (pk.max_slot == 0 && !layout->needsSyntheticSlot)
    s.perf.single_stream_draws++;
  auto *cmd = s.command_list;
  bool dynamic_state_invalid = s.bound_pipeline == nullptr;
  const bool pipeline_changed = s.bound_pipeline != pipeline;
  if (pipeline_changed) {
    cmd->setPipeline(pipeline);
    s.perf.pipeline_bind_calls++;
    if (memo_hit) {
      s.perf.pipeline_bind_on_hit++;
      static u32 logged = 0;
      if (logged++ < 8)
        EOT_DEBUG("[draw] pipeline bind on a memo hit: memo {} bound {} entry {} vs {:016x} ps {:016x} "
                  "samples {} rt0 {:#x}",
                  static_cast<const void *>(pipeline), static_cast<const void *>(s.bound_pipeline),
                  static_cast<const void *>(memo), vs->hash, ps ? ps->hash : 0, targets.samples,
                  targets.colorCount ? targets.color[0]->va : 0);
    }
    s.bound_pipeline = pipeline;
  }
#if defined(EOT_D3D12)
  static u32 last_stencil_ref = ~0u;
  if (dynamic_state_invalid)
    last_stencil_ref = ~0u;
  if (st.stencilEnable && last_stencil_ref != st.stencilRef) {
    static_cast<plume::D3D12CommandList *>(cmd)->d3d->OMSetStencilRef(st.stencilRef);
    last_stencil_ref = st.stencilRef;
    s.perf.stencil_ref_calls++;
  }
#endif
  static plume::RenderViewport last_vp;
  static plume::RenderRect last_sc;
  if (dynamic_state_invalid || std::memcmp(&last_vp, &vp.vp, sizeof(last_vp)) != 0) {
    cmd->setViewports(&vp.vp, 1);
    last_vp = vp.vp;
    s.perf.viewport_bind_calls++;
  }
  if (dynamic_state_invalid || std::memcmp(&last_sc, &vp.scissor, sizeof(last_sc)) != 0) {
    cmd->setScissors(&vp.scissor, 1);
    last_sc = vp.scissor;
    s.perf.scissor_bind_calls++;
  }
  const UploadAlloc *roots[2] = {&pk.vs_consts, &pk.ps_consts};
  auto bind_root_cbv = [&](u32 r, const UploadAlloc &alloc) {
#if defined(EOT_D3D12)
    if (alloc.gpuVa && s.root_cbv_index[r] != ~0u) {
      static_cast<plume::D3D12CommandList *>(cmd)->d3d->SetGraphicsRootConstantBufferView(
          s.root_cbv_index[r], alloc.gpuVa);
      return;
    }
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(alloc.buffer, alloc.offset), r);
#else
    const u64 address = alloc.gpuVa;
    cmd->setGraphicsPushConstants(kGuestPushConstantRangeIndex, &address, r * static_cast<u32>(sizeof(u64)),
                                  static_cast<u32>(sizeof(u64)));
#endif
  };
  for (u32 r = 0; r < 2; ++r) {
    if (s.bound_root_buffer[r] == roots[r]->buffer && s.bound_root_offset[r] == roots[r]->offset)
      continue;
    bind_root_cbv(r, *roots[r]);
    s.bound_root_buffer[r] = roots[r]->buffer;
    s.bound_root_offset[r] = roots[r]->offset;
  }
#if defined(EOT_D3D12)
  if (s.bound_spec != spec) {
    cmd->setGraphicsPushConstants(kSpecPushConstantRangeIndex, &spec, 0, sizeof(spec));
    s.bound_spec = spec;
  }
#endif
  if (s.bound_root_buffer[2] != shared_alloc.buffer ||
      s.bound_root_offset[2] != shared_alloc.offset) {
    bind_root_cbv(2, shared_alloc);
    s.bound_root_buffer[2] = shared_alloc.buffer;
    s.bound_root_offset[2] = shared_alloc.offset;
  }
  lap(s.perf.rec_state_ms);
  auto bind_vertex_views = [&](u32 first, const plume::RenderVertexBufferView *want_views,
                               u32 count, const plume::RenderInputSlot *want_slots) {
    s.perf.vertex_bind_requests++;
    u32 first_dirty = count, last_dirty = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 slot = first + i;
      const auto &want = want_views[i];
      const auto &input = want_slots[i];
      const auto &have = s.bound_vertex_streams[slot];
      if (!have.valid || have.buffer != want.buffer.ref || have.offset != want.buffer.offset ||
          have.size != want.size || have.stride != input.stride) {
        first_dirty = std::min(first_dirty, i);
        last_dirty = i;
      }
    }
    if (first_dirty == count)
      return;
    const u32 dirty_count = last_dirty - first_dirty + 1;
    cmd->setVertexBuffers(first + first_dirty, want_views + first_dirty, dirty_count,
                          want_slots + first_dirty);
    s.perf.vertex_bind_calls++;
    for (u32 i = first_dirty; i <= last_dirty; ++i) {
      const u32 slot = first + i;
      auto &have = s.bound_vertex_streams[slot];
      have.buffer = want_views[i].buffer.ref;
      have.offset = want_views[i].buffer.offset;
      have.size = want_views[i].size;
      have.stride = want_slots[i].stride;
      have.valid = true;
    }
  };
  bind_vertex_views(0, pk.views, pk.max_slot + 1, pk.slots);
  if (layout->needsSyntheticSlot) {
    plume::RenderVertexBufferView zv(plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset),
                                     4096);
    plume::RenderInputSlot zs(kSyntheticVertexSlot, 0);
    bind_vertex_views(kSyntheticVertexSlot, &zv, 1, &zs);
  }
  lap(s.perf.rec_bind_ms);
  auto bind_index_view = [&](const plume::RenderIndexBufferView &want) {
    s.perf.index_bind_requests++;
    auto &have = s.bound_index_stream;
    if (have.valid && have.buffer == want.buffer.ref && have.offset == want.buffer.offset &&
        have.size == want.size && have.format == want.format)
      return;
    cmd->setIndexBuffer(&want);
    s.perf.index_bind_calls++;
    have.buffer = want.buffer.ref;
    have.offset = want.buffer.offset;
    have.size = want.size;
    have.format = want.format;
    have.valid = true;
  };
  const u32 index_count = pk.index_count;
  if (pk.rectList) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else if (pk.hasCached) {
    const u32 elem = pk.cached.is32 ? 4 : 2;
    u64 capacity = 0;
    plume::RenderBuffer *chunk = PoolResidentBuffer(s, index_pool(), pk.cached.buffer,
                                                    pk.cached.offset, u64(index_count) * elem,
                                                    &capacity);
    const u64 view_bytes = capacity ? capacity : pk.cached.offset + u64(index_count) * elem;
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(chunk, 0), static_cast<u32>(std::min<u64>(view_bytes, 0xFFFFFFFFu)),
        pk.cached.is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, static_cast<u32>(pk.cached.offset / elem),
                              pk.host_base_vertex, 0);
  } else if (pk.indexed) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, pk.host_base_vertex, 0);
  } else {
    cmd->drawInstanced(pk.vertexCount, 1, static_cast<u32>(pk.host_base_vertex), 0);
  }
  lap(s.perf.record_ms);
  DrainHostDebugMessages(s, "draw");
}

void ExecuteDraw(u32 device_va, u32 prim, GeometryPlan &geom,
                 FloatConstantDirty constants, const u8 *device_image = nullptr) {
  auto &s = state();
  s.vs_float_constants_stale |= constants.vs;
  s.ps_float_constants_stale |= constants.ps;
  if (!s.ready)
    return;
  EOT_CPU_ZONE("ExecuteDraw");
  PerfScope perf_scope(s.perf.capture_ms);
  s.perf.draws++;
  if (RenderThreadActive()) {
    {
      RenderEnqueue enqueue;
      RenderCommand &c = enqueue.cmd();
      c.type = RenderCommandType::Draw;
      if (CaptureDraw(s, device_va, prim, geom, device_image, c.draw))
        enqueue.commit(false);
    }
    PoolDrainRetired(s, vertex_mirrors().pool);
    return;
  }
  static thread_local DrawPacket packet;
  const bool captured = CaptureDraw(s, device_va, prim, geom, device_image, packet);
  PoolDrainRetired(s, vertex_mirrors().pool);
  if (!captured)
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayDrawLocked(s, packet);
}

}

void ReplayDrawLocked(VideoState &s, const DrawPacket &pk) {
  if (!s.ready)
    return;
  PerfScope perf_scope(s.perf.draw_ms);
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  ReplayDraw(s, pk);
}

void FlushGeometryStaging(VideoState &s) {
  if (!s.command_list_open)
    return;
  PoolFlushToVram(s, index_pool(), plume::RenderBufferFlag::INDEX);
  PoolFlushToVram(s, vertex_mirrors().pool, plume::RenderBufferFlag::VERTEX);
}

void GeometryCacheBytes(GeometryCacheSizes *out) {
  *out = GeometryCacheSizes{};
  auto sum = [](BufferPool &pool, u64 &upload, u64 &vram, u32 &chunks) {
    std::lock_guard lock(pool.mutex);
    for (const auto &ch : pool.chunks) {
      upload += ch.buffer ? ch.capacity : 0;
      vram += ch.vram ? ch.capacity : 0;
      chunks++;
    }
  };
  sum(index_pool(), out->indexUpload, out->indexVram, out->indexChunks);
  sum(vertex_mirrors().pool, out->vertexUpload, out->vertexVram, out->vertexChunks);
}

void FlushPendingTransitions(VideoState &s) {
  if (!s.pending_transition_count)
    return;
  const u32 n = s.pending_transition_count;
  s.pending_transition_count = 0;
  TransitionManyLocked(s, s.pending_transitions, n);
}
namespace {

}

void DrawGuestPrimitives(u32 device_va, u32 prim, u32 start_vertex, u32 vertex_count,
                         FloatConstantDirty constants) {
  if (!vertex_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.startVertex = start_vertex;
  g.vertexCount = vertex_count;
  if (prim == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(vertex_count);
    for (u32 i = 0; i < vertex_count; ++i)
      g.indices[i] = start_vertex + i;
    ExpandIndices(prim, g.indices, false);
    g.indexed = true;
    g.baseVertex = 0;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  ExecuteDraw(device_va, prim, g, constants);
}

void QueueGuestUpDraw(u32 device_va, u32 prim, u32 vertex_count, u32 stride, u32 data_va,
                      FloatConstantDirty constants) {
  FlushPendingUpDraw();
  auto &s = state();
  std::lock_guard lock(s.guest_mutex);
  s.pending_up.valid = data_va != 0 && vertex_count != 0;
  s.pending_up_armed.store(s.pending_up.valid, std::memory_order_release);
  s.pending_up.device_va = device_va;
  s.pending_up.primitive_type = prim;
  s.pending_up.vertex_count = vertex_count;
  s.pending_up.stride = stride;
  s.pending_up.data_va = data_va;
  s.pending_up.vs_constants_dirty = constants.vs;
  s.pending_up.ps_constants_dirty = constants.ps;
  if (!s.pending_up.valid) {
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
  }
  s.pending_up.has_image = false;
  if (const u8 *image = mem::at<u8>(device_va)) {
    s.pending_up.device_image.assign(image, image + kDeviceSnapshotBytes);
    s.pending_up.has_image = true;
  }
}

void FlushPendingUpDraw() {
  auto &s = state();
  if (!s.pending_up_armed.load(std::memory_order_acquire))
    return;
  VideoState::PendingUpDraw up;
  {
    std::lock_guard lock(s.guest_mutex);
    if (!s.pending_up.valid)
      return;
    up = s.pending_up;
    s.pending_up.valid = false;
    s.pending_up_armed.store(false, std::memory_order_relaxed);
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(up.primitive_type, &expand);
  g.startVertex = 0;
  g.vertexCount = up.vertex_count;
  g.stream0OverrideVa = up.data_va;
  g.stream0OverrideStride = up.stride;
  if (up.primitive_type == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(up.vertex_count);
    for (u32 i = 0; i < up.vertex_count; ++i)
      g.indices[i] = i;
    ExpandIndices(up.primitive_type, g.indices, false);
    g.indexed = true;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  trace::Bump(trace::Counter::DrawVertices);
  ExecuteDraw(up.device_va, up.primitive_type, g,
              {up.vs_constants_dirty, up.ps_constants_dirty},
              up.has_image ? up.device_image.data() : nullptr);
}

void DrawGuestIndexedPrimitives(u32 device_va, u32 prim, i32 base_vertex, u32 start_index,
                                u32 index_count, FloatConstantDirty constants) {
  if (!index_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  auto &s = state();
  DeviceView dev = Device(device_va);
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.indexed = true;
  g.baseVertex = base_vertex;
  {
    PerfScope index_scope(s.perf.index_ms);
    if (const CachedIndexRange *cached =
            GetCachedIndexRange(s, dev.U32(dev::kIndexBuffer), prim, start_index, index_count)) {
      g.cached = cached;
      g.topology = cached->topology;
    }
  }
  if (g.cached) {
    ExecuteDraw(device_va, prim, g, constants);
    return;
  }
  {
    PerfScope index_scope(s.perf.index_ms);
    if (!ReadGuestIndices(dev.U32(dev::kIndexBuffer), start_index, index_count, g.indices)) {
      std::lock_guard lock(s.guest_mutex);
      s.vs_float_constants_stale |= constants.vs;
      s.ps_float_constants_stale |= constants.ps;
      Dropped("index buffer unreadable", 0x6020);
      return;
    }
    if (prim == kPrimRectList) {
      g.rectList = true;
    } else if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
      ExpandIndices(prim, g.indices, true);
      g.topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                          : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  }
  ExecuteDraw(device_va, prim, g, constants);
}

namespace {

bool CaptureClear(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil,
                  ClearPacket &pk) {
  pk = ClearPacket{};
  pk.device_va = device_va;
  pk.flags = flags;
  pk.z = z;
  pk.stencil = stencil;
  DeviceView dev = Device(device_va);
  if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
  }
  if (!CaptureTargetWords(dev, pk.targets))
    return false;
  if (rect_va) {
    pk.hasRect = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rect[i] = mem::load<i32>(rect_va + 4 * i);
  }
  if (color_va) {
    for (u32 i = 0; i < 4; ++i)
      pk.rgba[i] = mem::f32at(color_va + 4 * i);
  }
  return true;
}

}

void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil) {
  auto &s = state();
  if (!s.ready)
    return;
  if (RenderThreadActive()) {
    RenderEnqueue enqueue;
    RenderCommand &c = enqueue.cmd();
    c.type = RenderCommandType::Clear;
    if (CaptureClear(device_va, flags, rect_va, color_va, z, stencil, c.clear))
      enqueue.commit(false);
    return;
  }
  ClearPacket pk;
  if (!CaptureClear(device_va, flags, rect_va, color_va, z, stencil, pk))
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayClearLocked(s, pk);
}

void ReplayClearLocked(VideoState &s, const ClearPacket &pk) {
  if (!s.ready)
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  Targets targets;
  if (!ResolveTargetsFromWords(s, pk.targets, targets))
    return;
  const u32 flags = pk.flags, stencil = pk.stencil;
  const float z = pk.z;
  const float(&rgba)[4] = pk.rgba;
  const plume::RenderColor color(rgba[0], rgba[1], rgba[2], rgba[3]);
  plume::RenderRect rect;
  const plume::RenderRect *rects = nullptr;
  u32 rect_count = 0;
  bool whole = true;
  if (pk.hasRect) {
    const i32 gx0 = pk.rect[0], gy0 = pk.rect[1];
    const i32 gx1 = pk.rect[2], gy1 = pk.rect[3];
    whole = gx0 <= 0 && gy0 <= 0 && gx1 >= static_cast<i32>(targets.width) &&
            gy1 >= static_cast<i32>(targets.height);
    if (!whole) {
      const float k = targets.scale;
      rect = plume::RenderRect(ScalePxBy(gx0, k), ScalePxBy(gy0, k), ScalePxBy(gx1, k),
                               ScalePxBy(gy1, k));
      rects = &rect;
      rect_count = 1;
    }
  }
  const bool clear_depth = (flags & 0x10) != 0, clear_stencil = (flags & 0x20) != 0;
  if (clear_depth && whole && targets.depth)
    velocity::OnDepthCleared(s, *targets.depth);
  auto *cmd = s.command_list;
  if (GpuTimingDiagActive(s)) {
    GpuTimingDiagMark(s, cmd, std::format("clear flags {:#x}", flags));
    EOT_INFO("[diag] clear flags {:#x} {} rt0={:#x} ds={:#x} {}x{} rgba=({:.2f},{:.2f},{:.2f},{:.2f}) z={:.3f} s={}",
             flags, whole ? "whole" : "rect", targets.colorCount ? targets.color[0]->va : 0,
             targets.depth ? targets.depth->va : 0, targets.width, targets.height, rgba[0], rgba[1],
             rgba[2], rgba[3], z, stencil & 0xFF);
  }
  auto clears_all = [&](const HostTexture &image) {
    return clear_depth && (clear_stencil || !FormatHasStencil(image.format));
  };
  auto clear_depth_image = [&](const HostTexture &image, const plume::RenderRect *r, u32 n) {
    const bool with_stencil = clear_stencil && FormatHasStencil(image.format);
    if (clear_depth || with_stencil)
      cmd->clearDepthStencil(clear_depth, with_stencil, z, stencil & 0xFF, r, n);
  };
  auto clear_image = [&](GuestSurface &surf, HostTexture &image) {
    if (whole && (!surf.isDepth || clears_all(image)))
      image.needsClear = false;
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    if (!BindImages(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr, true))
      return;
    if (surf.isDepth)
      clear_depth_image(image, rects, rect_count);
    else
      cmd->clearColor(0, color, rects, rect_count);
    surf.perfClears++;
  };
  for (u32 i = 0; i < targets.colorCount; ++i) {
    if (!(flags & (1u << i)))
      continue;
    GuestSurface &surf = *targets.color[i];
    HostTexture &owner = SurfaceContentImage(surf);
    const bool both = SurfaceHasSingle(surf) &&
                      (whole || surf.imagesAgree || surf.content != GuestSurface::Content::Drawn);
    if (both) {
      if (surf.host.valid())
        clear_image(surf, surf.host);
      clear_image(surf, surf.single);
    } else {
      clear_image(surf, owner);
    }
    NoteSurfaceClearedColor(surf, owner, rgba, whole, both);
  }
  if (targets.depth && (flags & 0x30)) {
    GuestSurface &surf = *targets.depth;
    if (whole && !targets.colorCount && clear_depth)
      SurfaceRedirectBegin(s, surf);
    if (surf.redirectMirror && surf.redirectMirror->host.valid()) {
      HostTexture &image = surf.redirectMirror->host;
      plume::RenderRect region(surf.redirectX, surf.redirectY,
                               surf.redirectX + static_cast<i32>(surf.host.width),
                               surf.redirectY + static_cast<i32>(surf.host.height));
      if (!whole)
        region = plume::RenderRect(rect.left + surf.redirectX, rect.top + surf.redirectY,
                                   rect.right + surf.redirectX, rect.bottom + surf.redirectY);
      if (BindImages(s, nullptr, 0, &image, true)) {
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd, std::format("redirect region clear {}x{}", surf.host.width,
                                                surf.host.height));
        clear_depth_image(image, &region, 1);
        surf.perfClears++;
      }
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), false);
    } else {
      const bool both = SurfaceHasSingle(surf);
      if (surf.host.valid())
        clear_image(surf, surf.host);
      if (both)
        clear_image(surf, surf.single);
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), both);
    }
  }
  s.bound_draw_targets_valid = false;
  DrainHostDebugMessages(s, "clear");
}

}
