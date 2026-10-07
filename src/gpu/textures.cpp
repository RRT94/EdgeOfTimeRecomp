// gpu/textures.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/textures.h"
#include "gpu/render_thread.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <functional>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"

#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/format.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
using rex::graphics::FormatInfo;
using rex::graphics::TextureInfo;

struct UnlockTable {
  std::mutex mutex;
  std::unordered_map<u32, u64> seq;
  std::atomic<u64> global{0};
};

UnlockTable &unlocks() {
  static UnlockTable t;
  return t;
}

std::unordered_map<u32, TextureInfo> &infos() {
  static std::unordered_map<u32, TextureInfo> m;
  return m;
}

bool ReadFetch(u32 header_va, u32 out[6]) {
  auto *p = mem::at<be_u32>(header_va + obj::kTextureFetch);
  if (!p)
    return false;
  bool any = false;
  for (u32 i = 0; i < 6; ++i) {
    out[i] = p[i];
    any |= out[i] != 0;
  }
  return any;
}

u32 InfoWidth(const TextureInfo &info) { return info.width + 1; }
u32 InfoHeight(const TextureInfo &info) { return info.height + 1; }
u32 InfoDepth(const TextureInfo &info) { return info.depth + 1; }

bool SynthesizesMips(const TextureInfo &info, const TextureFormatMapping &m) {
  if (info.dimension != xe::DataDimension::k2DOrStacked || InfoDepth(info) > 1 ||
      info.mip_min_level != 0 || info.mip_max_level != 0)
    return false;
  const u32 w = InfoWidth(info), h = InfoHeight(info);
  if (w < 128 || h < 128 || (w & (w - 1)) || (h & (h - 1)))
    return false;
  return m.format == plume::RenderFormat::R8_UNORM || m.format == plume::RenderFormat::R8G8_UNORM ||
         m.format == plume::RenderFormat::R8G8B8A8_UNORM;
}

inline u32 TiledRowOffset(u32 y, u32 width, u32 log2_bpp) {
  const u32 macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  const u32 micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}

inline u32 TiledColumnOffset(u32 x, u32 y, u32 log2_bpp, u32 base_offset) {
  const u32 macro = (x / 32) << (log2_bpp + 7);
  const u32 micro = (x & 7) << log2_bpp;
  const u32 offset = base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

template <u32 kBpb, xe::Endian kEndian> inline void CopySwapBlockFixed(u8 *out, const u8 *in) {
  if constexpr (kEndian == xe::Endian::k8in16) {
    for (u32 i = 0; i < kBpb; i += 2) {
      u16 v;
      std::memcpy(&v, in + i, 2);
      v = static_cast<u16>((v >> 8) | (v << 8));
      std::memcpy(out + i, &v, 2);
    }
  } else if constexpr (kEndian == xe::Endian::k8in32) {
    for (u32 i = 0; i < kBpb; i += 4) {
      u32 v;
      std::memcpy(&v, in + i, 4);
      v = __builtin_bswap32(v);
      std::memcpy(out + i, &v, 4);
    }
  } else if constexpr (kEndian == xe::Endian::k16in32) {
    for (u32 i = 0; i < kBpb; i += 4) {
      u32 v;
      std::memcpy(&v, in + i, 4);
      v = (v >> 16) | (v << 16);
      std::memcpy(out + i, &v, 4);
    }
  } else {
    std::memcpy(out, in, kBpb);
  }
}

template <u32 kBpb, xe::Endian kEndian>
void UntileFixed(u8 *out, const u8 *in, const tc::UntileInfo &ui) {
  constexpr u32 kLog2Bpp = (kBpb / 4) + ((kBpb / 2) >> (kBpb / 4));
  const u64 out_pitch = u64(ui.output_pitch) * kBpb;
  for (u32 y = 0; y < ui.height; ++y) {
    const u32 gy = ui.offset_y + y;
    const u32 row = TiledRowOffset(gy, ui.input_pitch, kLog2Bpp);
    u8 *dst = out + y * out_pitch;
    for (u32 x = 0; x < ui.width; ++x, dst += kBpb) {
      const u32 block = TiledColumnOffset(ui.offset_x + x, gy, kLog2Bpp, row) >> kLog2Bpp;
      CopySwapBlockFixed<kBpb, kEndian>(dst, in + u64(block) * kBpb);
    }
  }
}

template <u32 kBpb> bool UntileForBpb(u8 *out, const u8 *in, const tc::UntileInfo &ui, xe::Endian endian) {
  switch (endian) {
  case xe::Endian::k8in16:
    if constexpr (kBpb % 2 == 0) {
      UntileFixed<kBpb, xe::Endian::k8in16>(out, in, ui);
      return true;
    }
    return false;
  case xe::Endian::k8in32:
    if constexpr (kBpb % 4 == 0) {
      UntileFixed<kBpb, xe::Endian::k8in32>(out, in, ui);
      return true;
    }
    return false;
  case xe::Endian::k16in32:
    if constexpr (kBpb % 4 == 0) {
      UntileFixed<kBpb, xe::Endian::k16in32>(out, in, ui);
      return true;
    }
    return false;
  default:
    UntileFixed<kBpb, xe::Endian::kNone>(out, in, ui);
    return true;
  }
}

bool UntileFast(u8 *out, const u8 *in, const tc::UntileInfo &ui, u32 bpb, xe::Endian endian) {
  switch (bpb) {
  case 1: return UntileForBpb<1>(out, in, ui, endian);
  case 2: return UntileForBpb<2>(out, in, ui, endian);
  case 4: return UntileForBpb<4>(out, in, ui, endian);
  case 8: return UntileForBpb<8>(out, in, ui, endian);
  case 16: return UntileForBpb<16>(out, in, ui, endian);
  default: return false;
  }
}

void UntileLevel(u8 *out, const u8 *in, const tc::UntileInfo &ui, u32 bpb, xe::Endian endian) {
  static u8 verdict[17][4] = {};
  const u32 e = static_cast<u32>(endian) & 3;
  u8 &state = verdict[std::min(bpb, 16u)][e];
  if (state != 2 && UntileFast(out, in, ui, bpb, endian)) {
    if (state == 1)
      return;
    const u64 pitch = u64(ui.output_pitch) * bpb;
    std::vector<u8> reference(pitch * ui.height + 64 * bpb, 0);
    tc::UntileInfo sdk = ui;
    sdk.copy_callback = [endian](void *o, const void *i, size_t n) { tc::CopySwapBlock(endian, o, i, n); };
    tc::Untile(reference.data(), in, &sdk);
    bool same = true;
    for (u32 y = 0; y < ui.height && same; ++y)
      same = std::memcmp(out + y * pitch, reference.data() + y * pitch, u64(ui.width) * bpb) == 0;
    state = same ? 1 : 2;
    if (same) {
      EOT_DEBUG("[textures] fast untile checked against the SDK's for {}-byte blocks, byte order {}", bpb, e);
    } else {
      EOT_ERROR("[textures] fast untile differs from the SDK's for {}-byte blocks, byte order {}; "
                "using the SDK's",
                bpb, e);
      std::memcpy(out, reference.data(), pitch * ui.height);
    }
    return;
  }
  tc::UntileInfo sdk = ui;
  sdk.copy_callback = [endian](void *o, const void *i, size_t n) { tc::CopySwapBlock(endian, o, i, n); };
  tc::Untile(out, in, &sdk);
}

void SynthesizeMipChain(VideoState &s, HostTexture &host, plume::RenderFormat format,
                        std::vector<u8> &level0, u32 w, u32 h, u64 pitch, u32 bpb) {
  std::vector<u8> prev = std::move(level0), cur;
  for (u32 level = 1; level < host.mipLevels; ++level) {
    const u32 lw = std::max(1u, w >> 1), lh = std::max(1u, h >> 1);
    const u64 lpitch = (u64(lw) * bpb + kTextureRowPitchAlignment - 1) /
                       kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 bytes = (lpitch * lh + kTexturePlacementAlignment - 1) /
                      kTexturePlacementAlignment * kTexturePlacementAlignment;
    cur.assign(bytes, 0);
    for (u32 y = 0; y < lh; ++y) {
      const u8 *r0 = prev.data() + u64(std::min(2 * y, h - 1)) * pitch;
      const u8 *r1 = prev.data() + u64(std::min(2 * y + 1, h - 1)) * pitch;
      u8 *dst = cur.data() + y * lpitch;
      for (u32 x = 0; x < lw; ++x) {
        const u64 x0 = u64(std::min(2 * x, w - 1)) * bpb, x1 = u64(std::min(2 * x + 1, w - 1)) * bpb;
        for (u32 c = 0; c < bpb; ++c)
          dst[x * bpb + c] = static_cast<u8>(
              (u32(r0[x0 + c]) + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
      }
    }
    UploadAlloc staging;
    if (!UploadAllocate(bytes, kTexturePlacementAlignment, &staging))
      return;
    std::memcpy(staging.cpu, cur.data(), bytes);
    GpuTimingMark(s, s.command_list, kGpuCatUpload);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging.buffer, format, lw, lh, 1, static_cast<u32>(lpitch / bpb), staging.offset),
        0, 0, 0);
    prev.swap(cur);
    w = lw;
    h = lh;
    pitch = lpitch;
  }
  host.needsClear = false;
}

bool CreateHostImage(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  HostTexture &host = t.host;
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (!m.supported) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: unsupported guest format {} ({}x{}), sampling null", t.va,
               static_cast<u32>(info.format), InfoWidth(info), InfoHeight(info));
    }
    return false;
  }
  const bool depth = info.format == xe::TextureFormat::k_24_8 ||
                     info.format == xe::TextureFormat::k_24_8_FLOAT;

  plume::RenderTextureDesc desc;
  desc.width = InfoWidth(info);
  desc.height = InfoHeight(info);
  if (m.blockCompressed) {
    desc.width = (desc.width + 3) & ~3u;
    desc.height = (desc.height + 3) & ~3u;
  }
  desc.depth = 1;
  desc.arraySize = 1;
  desc.committed = false;
  switch (info.dimension) {
  case xe::DataDimension::k3D:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_3D;
    desc.depth = InfoDepth(info);
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_3D;
    break;
  case xe::DataDimension::kCube:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.arraySize = 6;
    desc.flags = plume::RenderTextureFlag::CUBE;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
    break;
  case xe::DataDimension::k1D:
  case xe::DataDimension::k2DOrStacked:
  default:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    if (info.is_stacked && InfoDepth(info) > 1)
      desc.arraySize = InfoDepth(info);
    break;
  }
  u32 max_levels = 1;
  for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14; ++max_levels) {
    w = std::max(1u, w >> 1);
    h = std::max(1u, h >> 1);
  }
  t.synthMips = !depth && SynthesizesMips(info, m);
  desc.mipLevels = t.synthMips ? max_levels : std::min(info.mip_max_level + 1u, max_levels);
  if (t.synthMips)
    EOT_DEBUG("[textures] {:#x}: {}x{} fmt {} ships one level; the host carries {}", t.va,
              desc.width, desc.height, static_cast<u32>(info.format), desc.mipLevels);

  if (depth) {
    desc.format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    desc.flags = desc.flags | plume::RenderTextureFlag::DEPTH_TARGET;
    host.isDepth = true;
  } else {
    desc.format = t.gammaSigned && m.srgbFormat != plume::RenderFormat::UNKNOWN ? m.srgbFormat : m.format;
    if (!m.blockCompressed && desc.dimension != plume::RenderTextureDimension::TEXTURE_3D)
      desc.flags = desc.flags | plume::RenderTextureFlag::RENDER_TARGET;
    host.isDepth = false;
  }
  host.format = desc.format;
  host.width = desc.width;
  host.height = desc.height;
  host.depth = desc.depth;
  host.mipLevels = desc.mipLevels;
  host.arraySize = desc.arraySize;
  host.renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                    (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  CreateOrRecycleHostTexture(s, host, desc, "guest-texture");
  if (!host.texture) {
    t.uploadFailed = true;
    return false;
  }
  return true;
}

void UploadFromGuest(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  t.storeSwapRB = false;
  TextureReleaseBorrower(s, t);
  FlushAliasDependents(s, t);
  t.aliasPending = false;
  t.contentSerial++;
  EOT_CPU_ZONE("texture upload");
  HostTexture &host = t.host;
  if (!host.texture || host.isDepth)
    return;
  PerfScope perf_scope(s.perf.upload_ms);
  s.perf.uploads++;
  if (t.uploaded) {
    s.perf.uploads_refresh++;
  } else if (UploadSeenBefore((u64(info.memory.base_address) << 32) ^ (u64(t.fetch[1]) << 16) ^ t.fetch[2] ^
                              (u64(info.memory.mip_address) << 7))) {
    s.perf.uploads_again++;
    f64 age_ms = 0;
    if (TakeTextureResidentAge(t.va, age_ms))
      s.perf.uploads_again_reloaded++;
  } else {
    s.perf.uploads_new++;
    f64 age_ms = 0;
    const u32 bucket = !TakeTextureResidentAge(t.va, age_ms) ? 4
                       : age_ms < 50.0                       ? 0
                       : age_ms < 250.0                      ? 1
                       : age_ms < 1000.0                     ? 2
                                                             : 3;
    s.perf.upload_lead[bucket]++;
    if (bucket == 4)
      NoteUnannouncedUpload(InfoWidth(info), InfoHeight(info), static_cast<u32>(info.format), info.is_tiled);
  }
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (m.convert) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: guest format {} needs a texel conversion the uploader "
               "lacks yet; sampling whatever is there",
               t.va, static_cast<u32>(info.format));
    }
    return;
  }
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, t.fetch, sizeof(fetch));
  const FormatInfo *fi = info.format_info();
  const u32 bpb = fi->bytes_per_block();
  const u32 slices = host.arraySize > 1 ? host.arraySize : host.depth;
  const bool is_3d = info.dimension == xe::DataDimension::k3D;
  const bool has_base = info.memory.base_address != 0;
  const u32 width = InfoWidth(info), height = InfoHeight(info), depth = InfoDepth(info);
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      info.dimension, fetch.pitch, width, height, is_3d ? depth : slices, info.is_tiled,
      info.format, info.has_packed_mips, has_base, info.mip_max_level);

  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_DEST);
  for (u32 level = info.mip_min_level; level <= info.mip_max_level && level < host.mipLevels;
       ++level) {
    u32 address = 0;
    const tu::TextureGuestLayout::Level *lvl = nullptr;
    u32 x_blocks = 0, y_blocks = 0, z_blocks = 0;
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    if (level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else if (!info.memory.mip_address && layout.packed_level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else {
      if (!info.memory.mip_address)
        continue;
      const u32 stored = packed ? layout.packed_level : level;
      address = info.memory.mip_address + layout.mip_offsets_bytes[stored];
      lvl = &layout.mips[stored];
    }
    if (packed) {
      tu::GetPackedMipOffset(width, height, is_3d ? depth : 1, info.format, level, x_blocks,
                             y_blocks, z_blocks);
    }
    u32 w = 0, h = 0;
    info.GetMipSize(level, &w, &h);
    if (!address || !w || !h || !lvl->row_pitch_bytes)
      continue;
    if (m.blockCompressed) {
      w = std::max(4u, (w + 3) & ~3u);
      h = std::max(4u, (h + 3) & ~3u);
    }
    if (w > std::max(1u, host.width >> level) || h > std::max(1u, host.height >> level)) {
      w = std::max(1u, host.width >> level);
      h = std::max(1u, host.height >> level);
    }
    if (!mem::readable(address, lvl->level_data_extent_bytes)) {
      u32 n;
      if (DiagShouldLog(0x5EB0 ^ t.va, &n))
        EOT_WARN("[textures] {:#x}: level {} at {:#x} ({} bytes) is not mapped guest memory; "
                 "skipping the upload (x{})",
                 t.va, level, address, lvl->level_data_extent_bytes, n + 1);
      continue;
    }
    const u8 *src_base = mem::at<u8>(address);
    if (!src_base)
      continue;
    const u32 bw = (w + fi->block_width - 1) / fi->block_width;
    const u32 bh = (h + fi->block_height - 1) / fi->block_height;
    const u32 guest_pitch_blocks = lvl->row_pitch_bytes / bpb;
    const u64 host_pitch = (u64(bw) * bpb + kTextureRowPitchAlignment - 1) /
                           kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 host_slice_bytes = (host_pitch * bh + kTexturePlacementAlignment - 1) /
                                 kTexturePlacementAlignment * kTexturePlacementAlignment;
    const u64 guest_slice_bytes =
        is_3d ? u64(lvl->z_slice_stride_block_rows) * lvl->row_pitch_bytes
              : lvl->array_slice_stride_bytes;
    UploadAlloc staging;
    if (!UploadAllocate(host_slice_bytes * slices, kTexturePlacementAlignment, &staging))
      return;
    std::vector<u8> scratch;
    if (t.synthMips && level == 0 && slices == 1 && host.mipLevels > 1)
      scratch.assign(host_slice_bytes, 0);
    for (u32 slice = 0; slice < slices; ++slice) {
      u8 *dst = scratch.empty() ? staging.cpu + slice * host_slice_bytes : scratch.data();
      const u8 *src = src_base + slice * guest_slice_bytes +
                      (is_3d ? u64(z_blocks) * lvl->row_pitch_bytes * lvl->z_slice_stride_block_rows
                             : 0);
      if (info.is_tiled && is_3d) {
        u32 bpb_log2 = 0;
        while ((1u << bpb_log2) < bpb)
          ++bpb_log2;
        for (u32 y = 0; y < bh; ++y) {
          u8 *dst_row = dst + y * host_pitch;
          for (u32 x = 0; x < bw; ++x) {
            const i32 off = tu::GetTiledOffset3D(
                static_cast<i32>(x + x_blocks), static_cast<i32>(y + y_blocks),
                static_cast<i32>(slice + z_blocks), guest_pitch_blocks,
                lvl->z_slice_stride_block_rows, bpb_log2);
            tc::CopySwapBlock(info.endianness, dst_row + x * bpb, src_base + off, bpb);
          }
        }
      } else if (info.is_tiled) {
        tc::UntileInfo ui{};
        ui.offset_x = x_blocks;
        ui.offset_y = y_blocks;
        ui.width = bw;
        ui.height = bh;
        ui.input_pitch = guest_pitch_blocks;
        ui.output_pitch = static_cast<u32>(host_pitch / bpb);
        ui.input_format_info = fi;
        ui.output_format_info = fi;
        UntileLevel(dst, src, ui, bpb, info.endianness);
      } else {
        const u8 *row = src + u64(y_blocks) * lvl->row_pitch_bytes + u64(x_blocks) * bpb;
        for (u32 y = 0; y < bh; ++y) {
          tc::CopySwapBlock(info.endianness, dst + y * host_pitch, row + y * lvl->row_pitch_bytes,
                            u64(bw) * bpb);
        }
      }
      if (!scratch.empty())
        std::memcpy(staging.cpu, scratch.data(), host_slice_bytes);
      const u32 row_width_texels = static_cast<u32>(host_pitch / bpb) * fi->block_width;
      GpuTimingMark(s, s.command_list, kGpuCatUpload);
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level,
                                                        host.arraySize > 1 ? slice : 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(
              staging.buffer, m.format, w, h, 1, row_width_texels,
              staging.offset + slice * host_slice_bytes),
          0, 0, is_3d ? slice : 0);
    }
    s.perf.upload_blocks += u64(bw) * bh * slices;
    if (!scratch.empty()) {
      PerfScope synth_scope(s.perf.upload_synth_ms);
      SynthesizeMipChain(s, host, m.format, scratch, w, h, host_pitch, bpb);
    }
  }
  t.uploaded = true;
  if (info.mip_min_level == 0 && info.mip_max_level + 1 >= host.mipLevels)
    host.needsClear = false;
}

}

constexpr f64 kTextureIdleSeconds = 30.0;
constexpr f64 kUploadedTextureIdleSeconds = 300.0;
constexpr size_t kTextureCap = 4096;

static bool ReadyToRetire(VideoState &s, const std::shared_ptr<GuestTexture> &slot) {
  if (slot.use_count() > 1)
    SurfacesReleaseMirror(s, *slot);
  if (slot.use_count() > 1 || (!slot->borrower && slot->aliasDependents.empty()))
    return true;
  BeginCommandList(s);
  return s.command_list_open;
}

void EvictStaleGuestTextures(VideoState &s) {
  EOT_CPU_ZONE("evict guest textures");
  bool evicted = false;
  {
    static std::vector<u32> released;
    released.clear();
    TakeReleasedTextures(released);
    for (u32 va : released) {
      const auto it = s.textures.find(va);
      if (it == s.textures.end() || !it->second || it->second->resolveOwned || !ReadyToRetire(s, it->second))
        continue;
      if (it->second.use_count() == 1) {
        TextureReleaseBorrower(s, *it->second);
        FlushAliasDependents(s, *it->second);
        ParkHostTexture(s, it->second->host);
      }
      infos().erase(it->first);
      s.textures.erase(it);
      s.perf.textures_evicted++;
      s.perf.textures_released++;
      evicted = true;
    }
  }
  u64 oldest_kept_frame = 0;
  if (s.textures.size() > kTextureCap) {
    std::vector<u64> frames;
    frames.reserve(s.textures.size());
    for (const auto &kv : s.textures)
      frames.push_back(kv.second ? kv.second->lastUseFrame : 0);
    std::nth_element(frames.begin(), frames.end() - kTextureCap, frames.end());
    oldest_kept_frame = frames[frames.size() - kTextureCap];
  }
  for (auto it = s.textures.begin(); it != s.textures.end();) {
    const std::shared_ptr<GuestTexture> &slot = it->second;
    if (!slot) {
      infos().erase(it->first);
      it = s.textures.erase(it);
      s.perf.textures_evicted++;
      evicted = true;
      continue;
    }
    const f64 idle_seconds = slot->resolveOwned ? kTextureIdleSeconds : kUploadedTextureIdleSeconds;
    if ((FrameAgeSeconds(s, slot->lastUseFrame) < idle_seconds && slot->lastUseFrame >= oldest_kept_frame) ||
        !ReadyToRetire(s, slot)) {
      ++it;
      continue;
    }
    if (slot.use_count() == 1) {
      TextureReleaseBorrower(s, *slot);
      FlushAliasDependents(s, *slot);
      ParkHostTexture(s, slot->host);
    }
    infos().erase(it->first);
    it = s.textures.erase(it);
    s.perf.textures_evicted++;
    evicted = true;
  }
  if (evicted)
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
}

void PreloadAnnouncedTextures(VideoState &s, f64 budget_ms) {
  if (!s.command_list_open || budget_ms <= 0.0)
    return;
  const u64 start = PerfNow();
  const f64 ms_per_tick = PerfMsPerTick();
  u32 header = 0;
  while (static_cast<f64>(PerfNow() - start) * ms_per_tick < budget_ms && TakeAnnouncedHeader(header)) {
    if (s.textures.count(header))
      continue;
    GuestTexture *t = GetGuestTexture(s, header);
    if (!t || t->resolveOwned || !t->host.texture || t->host.isDepth)
      continue;
    const u32 before = s.perf.uploads;
    PrepareTextureForSampling(s, *t);
    s.perf.uploads_preloaded += s.perf.uploads - before;
  }
}

void NotifyResourceUnlocked(u32 resource_va) {
  auto &u = unlocks();
  {
    std::lock_guard lock(u.mutex);
    const u64 next = u.global.load(std::memory_order_relaxed) + 1;
    u.seq[resource_va] = next;
    u.global.store(next, std::memory_order_release);
  }
  auto &s = state();
  if (RenderThreadActive()) {
    RenderThreadUnlock(resource_va);
    return;
  }
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (lock.owns_lock()) {
    NotifyResourceUnlockedLocked(s, resource_va);
  } else {
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

void NotifyResourceUnlockedLocked(VideoState &s, u32 resource_va) {
  const auto texture = s.textures.find(resource_va);
  if (texture != s.textures.end() && texture->second)
    texture->second->bindingGeneration++;
}

u64 ResourceUnlockSeq(u32 resource_va) {
  auto &u = unlocks();
  struct Cached {
    u32 va = 0;
    u64 seq = 0;
    u64 global = ~0ull;
  };
  static thread_local Cached cache[8];
  const u64 global = u.global.load(std::memory_order_acquire);
  Cached &c = cache[(resource_va >> 5) & 7];
  if (c.va == resource_va && c.global == global)
    return c.seq;
  u64 seq = 0;
  {
    std::lock_guard lock(u.mutex);
    auto it = u.seq.find(resource_va);
    seq = it == u.seq.end() ? 0 : it->second;
  }
  c = {resource_va, seq, global};
  return seq;
}

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image) {
  if (!header_va)
    return nullptr;
  u32 fetch[6];
  if (!ReadFetch(header_va, fetch))
    return nullptr;
  auto &slot = s.textures[header_va];
  auto same_storage = [](const u32 *a, const u32 *b) {
    auto fmt = [](u32 d1) {
      const u32 f = d1 & 0x3F;
      return (d1 & ~0x3Fu) | (f == 50 ? 6u : f);
    };
    return (a[0] & ~0x7FFFCu) == (b[0] & ~0x7FFFCu) && fmt(a[1]) == fmt(b[1]) &&
           a[2] == b[2] && a[5] == b[5];
  };
  if (slot && same_storage(slot->fetch, fetch)) {
    if (std::memcmp(slot->fetch, fetch, sizeof(fetch)) != 0)
      std::memcpy(slot->fetch, fetch, sizeof(fetch));
    if (create_host_image && !slot->host.texture) {
      const auto info = infos().find(header_va);
      if (info != infos().end() && CreateHostImage(s, *slot, info->second))
        slot->bindingGeneration++;
    }
    return slot.get();
  }

  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fetch, sizeof(f));
  TextureInfo info{};
  if (!TextureInfo::Prepare(f, &info)) {
    u32 n;
    if (DiagShouldLog(0x5E00 ^ header_va, &n))
      EOT_WARN("[textures] {:#x}: TextureInfo::Prepare rejected the fetch constant", header_va);
    return nullptr;
  }
  if (slot) {
    u32 n;
    if (DiagShouldLog(0x5EA0 ^ header_va, &n))
      EOT_DEBUG("[textures] {:#x}: header changed ({}x{} fmt {} base {:#x} -> {}x{} fmt {} base "
               "{:#x}), recreating (x{})",
               header_va, slot->width, slot->height, static_cast<u32>(slot->format),
               slot->baseAddress, InfoWidth(info), InfoHeight(info),
               static_cast<u32>(info.format), info.memory.base_address, n + 1);
    if (slot.use_count() > 1)
      SurfacesReleaseMirror(s, *slot);
    if (slot.use_count() == 1) {
      TextureReleaseBorrower(s, *slot);
      FlushAliasDependents(s, *slot);
      ParkHostTexture(s, slot->host);
    }
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
    slot.reset();
  }
  for (auto &[other_va, other] : s.textures) {
    if (!other || other_va == header_va || !(other->resolveOwned || other->host.isDepth) ||
        !same_storage(other->fetch, fetch))
      continue;
    u32 n;
    if (DiagShouldLog(0x5EB0 ^ header_va, &n))
      EOT_DEBUG("[textures] {:#x}: aliases {:#x} ({}x{} fmt {} base {:#x})", header_va, other_va,
               other->width, other->height, static_cast<u32>(other->format), other->baseAddress);
    infos()[header_va] = info;
    slot = other;
    return slot.get();
  }
  auto t = std::make_shared<GuestTexture>();
  t->va = header_va;
  std::memcpy(t->fetch, fetch, sizeof(fetch));
  t->format = info.format;
  t->dimension = info.dimension;
  t->width = InfoWidth(info);
  t->height = InfoHeight(info);
  t->depth = InfoDepth(info);
  t->mipLevels = info.mip_levels();
  t->tiled = info.is_tiled;
  t->baseAddress = info.memory.base_address;
  t->mipAddress = info.memory.mip_address;
  t->gammaSigned = f.sign_x == xe::TextureSign::kGamma;
  infos()[header_va] = info;
  if (create_host_image)
    CreateHostImage(s, *t, info);
  EOT_DEBUG("[textures] {:#x}: {}x{}x{} mips {}..{} fmt {} {} {} base {:#x} mip {:#x} -> host fmt {}",
            header_va, InfoWidth(info), InfoHeight(info), InfoDepth(info), info.mip_min_level,
            info.mip_max_level, static_cast<u32>(info.format), info.is_tiled ? "tiled" : "linear",
            t->gammaSigned ? "gamma" : "linear-sign", t->baseAddress, t->mipAddress,
            static_cast<u32>(t->host.format));
  slot = std::move(t);
  return slot.get();
}

u32 SamplingSwizzle(const GuestTexture &t, u32 fetch_swizzle) {
  fetch_swizzle &= 0xFFF;
  if (!t.storeSwapRB)
    return fetch_swizzle;
  u32 out = 0;
  for (u32 c = 0; c < 4; ++c) {
    u32 sel = (fetch_swizzle >> (3 * c)) & 7;
    if (sel == 0)
      sel = 2;
    else if (sel == 2)
      sel = 0;
    out |= sel << (3 * c);
  }
  return out;
}

static std::pair<u32, u32> BaseRange(const GuestTexture &t) {
  const auto it = infos().find(t.va);
  if (it == infos().end() || !t.baseAddress)
    return {0, 0};
  return {t.baseAddress, t.baseAddress + std::max<u32>(it->second.memory.base_size, 1)};
}

static bool InResolveMemory(const VideoState &s, const GuestTexture &t) {
  const auto [start, end] = BaseRange(t);
  if (start >= end || s.resolve_ranges.empty())
    return false;
  auto it = s.resolve_ranges.upper_bound(start);
  if (it != s.resolve_ranges.begin() && std::prev(it)->second > start)
    return true;
  return it != s.resolve_ranges.end() && it->first < end;
}

void NoteResolveDestination(VideoState &s, const GuestTexture &t) {
  auto [start, end] = BaseRange(t);
  if (start >= end)
    return;
  auto &ranges = s.resolve_ranges;
  auto it = ranges.upper_bound(start);
  if (it != ranges.begin() && std::prev(it)->second >= start)
    --it;
  while (it != ranges.end() && it->first <= end) {
    start = std::min(start, it->first);
    end = std::max(end, it->second);
    it = ranges.erase(it);
  }
  ranges.emplace(start, end);
}

static bool TakeOverAsResolveTarget(VideoState &s, GuestTexture &t) {
  if (t.host.isDepth || !s.command_list_open)
    return false;
  if (t.host.texture && !t.host.renderable) {
    ParkHostTexture(s, t.host);
    t.host = HostTexture{};
    t.bindingGeneration++;
  }
  if (!EnsureResolveMirror(s, t, false) || !t.host.renderable)
    return false;
  for (u32 m = 0; m < t.host.mipLevels; ++m) {
    plume::RenderFramebuffer *fb = GetMipFramebuffer(s, t, m);
    if (!fb)
      continue;
    TransitionLocked(s, t.host, plume::RenderTextureLayout::COLOR_WRITE);
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
  }
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  t.host.needsClear = false;
  t.resolveOwned = true;
  t.resolveProvisional = true;
  t.uploaded = true;
  s.perf.uploads_skipped++;
  return true;
}

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t, u32 swizzle) {
  if (!t.host.texture)
    return kInvalidDescriptorIndex;
  swizzle = SamplingSwizzle(t, swizzle);
  const u64 seq = ResourceUnlockSeq(t.va);
  const bool stale = !t.uploaded || seq != t.uploadedUnlockSeq;
  if (stale && t.resolveOwned) {
    u32 n;
    if (seq > t.uploadedUnlockSeq && DiagShouldLog(0x5E80 ^ t.va, &n))
      EOT_DEBUG("[textures] {:#x}: Unlock on a resolve-owned mirror ignored (seq {} -> {})", t.va,
               t.uploadedUnlockSeq, seq);
    t.uploadedUnlockSeq = seq;
  } else if (stale && !t.uploaded && InResolveMemory(s, t) && TakeOverAsResolveTarget(s, t)) {
    t.uploadedUnlockSeq = seq;
  } else if (stale) {
    auto it = infos().find(t.va);
    if (it != infos().end()) {
      UploadFromGuest(s, t, it->second);
      t.uploadedUnlockSeq = seq;
    }
  }
  t.lastUseFrame = s.guest_frames;
  TransitionLocked(s, t.host, plume::RenderTextureLayout::SHADER_READ);
  return BindTextureSRVSwizzledLocked(s, t.host, swizzle);
}

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale,
                         plume::RenderFormat depth_format) {
  const float k = scale > 0.0f ? scale : RenderScaleFactor();
  const auto info_it = infos().find(t.va);
  if (info_it == infos().end())
    return false;
  const TextureInfo &info = info_it->second;

  const TextureFormatMapping mapping = MapTextureFormat(t.format);
  const bool texture_is_depth = t.format == xe::TextureFormat::k_24_8 ||
                                t.format == xe::TextureFormat::k_24_8_FLOAT;
  if (!t.host.texture &&
      (t.dimension != xe::DataDimension::k2DOrStacked ||
       (info.is_stacked && InfoDepth(info) > 1) || !mapping.supported ||
       mapping.blockCompressed || depth_source != texture_is_depth)) {
    if (!CreateHostImage(s, t, info))
      return false;
    t.bindingGeneration++;
  }
  if ((!t.resolveOwned || t.resolveProvisional) &&
      (!t.host.texture || (depth_source == t.host.isDepth && t.host.renderable))) {
    const plume::RenderFormat want = depth_source && depth_format != plume::RenderFormat::UNKNOWN
                                         ? depth_format
                                         : ResolveDestinationFormat(t.format, depth_source);
    if (want == plume::RenderFormat::UNKNOWN)
      return false;
    const u32 want_w = ScaleDimBy(InfoWidth(info), k), want_h = ScaleDimBy(InfoHeight(info), k);
    if (k != RenderScaleFactor() && (t.host.width != want_w || t.host.height != want_h))
      EOT_DEBUG("[textures] {:#x}: resolve mirror at x{:.2f}: {}x{}", t.va, k, want_w, want_h);
    if (!t.host.texture || t.host.format != want || t.host.width != want_w ||
        t.host.height != want_h) {
      EOT_DEBUG("[textures] {:#x}: switching mirror to resolve format {} {}x{} (was {} {}x{})",
                t.va, static_cast<u32>(want), want_w, want_h, static_cast<u32>(t.host.format),
                t.host.width, t.host.height);
      const bool replacing_host = t.host.texture != nullptr;
      TextureReleaseBorrower(s, t);
      FlushAliasDependents(s, t);
      ParkHostTexture(s, t.host);
      t.aliasPending = false;
      t.bindingGeneration++;
      s.mirror_generation++;
      const bool is_depth = depth_source;
      t.host = HostTexture{};
      t.synthMips = false;
      plume::RenderTextureDesc desc;
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      desc.width = want_w;
      desc.height = want_h;
      desc.depth = 1;
      desc.arraySize = 1;
      u32 max_levels = 1;
      for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14;
           ++max_levels) {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
      }
      desc.mipLevels = is_depth && replacing_host
                           ? 1u
                           : std::min(info.mip_max_level + 1u, max_levels);
      desc.format = ImageResourceFormat(want);
      desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
      const u32 gw = InfoWidth(info), gh = InfoHeight(info);
      bool frame_shape = gw >= 2048 && gh >= 2048;
      bool near_frame = false;
      for (u32 k = 0; k < 4; ++k) {
        const i32 fw = static_cast<i32>(kGuestRenderWidth >> k), fh = static_cast<i32>(kGuestRenderHeight >> k);
        frame_shape |= gw == static_cast<u32>(fw) && gh == static_cast<u32>(fh);
        near_frame |= std::abs(static_cast<i32>(gw) - fw) <= 2 && std::abs(static_cast<i32>(gh) - fh) <= 2;
      }
      desc.committed = frame_shape && u64(desc.width) * desc.height >= 256ull * 256ull;
      plume::RenderPool *pool = !frame_shape && !near_frame && gw > 256 && gh > 256
                                    ? s.transient_mirror_pool.get()
                                    : nullptr;
      t.host.format = want;
      t.host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      t.host.width = desc.width;
      t.host.height = desc.height;
      t.host.depth = 1;
      t.host.mipLevels = desc.mipLevels;
      t.host.arraySize = 1;
      t.host.isDepth = is_depth;
      CreateOrRecycleHostTexture(s, t.host, desc, "resolve-mirror", pool);
      t.host.renderable = t.host.texture != nullptr;
      t.uploaded = false;
      if (!t.host.texture)
        return false;
    }
  }
  if (!t.host.renderable) {
    u32 n;
    if (DiagShouldLog(0x5F00 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: resolve into a non-renderable mirror (fmt {})", t.va,
               static_cast<u32>(t.host.format));
    return false;
  }
  if (depth_source != t.host.isDepth) {
    u32 n;
    if (DiagShouldLog(0x5F80 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: {} resolve into a {} mirror", t.va,
               depth_source ? "depth" : "colour", t.host.isDepth ? "depth" : "colour");
    return false;
  }
  return true;
}

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip) {
  HostTexture &host = t.host;
  if (!host.texture || mip >= host.mipLevels)
    return nullptr;
  if (host.mipFramebuffers.size() < host.mipLevels) {
    host.mipFramebuffers.resize(host.mipLevels);
    host.mipViews.resize(host.mipLevels);
  }
  if (host.mipFramebuffers[mip])
    return host.mipFramebuffers[mip].get();
  plume::RenderTextureViewDesc vd;
  vd.format = host.format;
  vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipSlice = mip;
  vd.mipLevels = 1;
  vd.arrayIndex = 0;
  vd.arraySize = 1;
  s.perf.host_views++;
  host.mipViews[mip] = host.texture->createTextureView(vd);
  if (!host.mipViews[mip])
    return nullptr;
  plume::RenderFramebufferDesc fd;
  const plume::RenderTexture *color[1] = {host.texture.get()};
  const plume::RenderTextureView *views[1] = {host.mipViews[mip].get()};
  if (host.isDepth) {
    fd.depthAttachment = host.texture.get();
    fd.depthAttachmentView = host.mipViews[mip].get();
  } else {
    fd.colorAttachments = color;
    fd.colorAttachmentViews = views;
    fd.colorAttachmentsCount = 1;
  }
  host.mipFramebuffers[mip] = s.device->createFramebuffer(fd);
  return host.mipFramebuffers[mip].get();
}

}

namespace eot::gpu {

namespace {

struct DescKey {
  plume::RenderTextureAddressMode u, v, w;
  plume::RenderFilter min, mag;
  plume::RenderMipmapMode mip;
  plume::RenderBorderColor border;
  u32 maxAniso;
  bool anisoEnabled;
  float lodBias;
  float minLod;
  float maxLod;

  bool operator==(const DescKey &o) const noexcept {
    return u == o.u && v == o.v && w == o.w && min == o.min && mag == o.mag && mip == o.mip &&
           border == o.border && maxAniso == o.maxAniso && anisoEnabled == o.anisoEnabled &&
           lodBias == o.lodBias && minLod == o.minLod && maxLod == o.maxLod;
  }
};

struct DescKeyHash {
  size_t operator()(const DescKey &k) const noexcept {
    u64 bits = 0;
    bits |= static_cast<u64>(k.u);
    bits |= static_cast<u64>(k.v) << 4;
    bits |= static_cast<u64>(k.w) << 8;
    bits |= static_cast<u64>(k.min) << 12;
    bits |= static_cast<u64>(k.mag) << 16;
    bits |= static_cast<u64>(k.mip) << 20;
    bits |= static_cast<u64>(k.border) << 24;
    bits |= static_cast<u64>(k.maxAniso) << 28;
    bits |= static_cast<u64>(k.anisoEnabled) << 36;
    u32 lb, mn, mx;
    std::memcpy(&lb, &k.lodBias, 4);
    std::memcpy(&mn, &k.minLod, 4);
    std::memcpy(&mx, &k.maxLod, 4);
    return std::hash<u64>{}(bits ^ (u64(lb) << 13) ^ (u64(mn) << 29) ^ (u64(mx) << 41));
  }
};

struct Cache {
  std::unordered_map<DescKey, std::pair<std::unique_ptr<plume::RenderSampler>, u32>, DescKeyHash>
      map;
};

Cache &cache() {
  static Cache c;
  return c;
}

plume::RenderTextureAddressMode ConvertClamp(xe::ClampMode mode) {
  using A = plume::RenderTextureAddressMode;
  switch (mode) {
  case xe::ClampMode::kRepeat:
    return A::WRAP;
  case xe::ClampMode::kMirroredRepeat:
    return A::MIRROR;
  case xe::ClampMode::kClampToEdge:
  case xe::ClampMode::kClampToHalfway:
    return A::CLAMP;
  case xe::ClampMode::kMirrorClampToEdge:
  case xe::ClampMode::kMirrorClampToHalfway:
  case xe::ClampMode::kMirrorClampToBorder:
    return A::MIRROR_ONCE;
  case xe::ClampMode::kClampToBorder:
    return A::BORDER;
  }
  return A::CLAMP;
}

}

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload,
                                                bool synthesized_mips) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));
  const bool base_map = fetch.mip_filter == xe::TextureFilter::kBaseMap && !synthesized_mips;

  plume::RenderSamplerDesc d;
  d.addressU = ConvertClamp(fetch.clamp_x);
  d.addressV = ConvertClamp(fetch.clamp_y);
  d.addressW = ConvertClamp(fetch.clamp_z);
  const bool mag_point = fetch.mag_filter == xe::TextureFilter::kPoint;
  const bool min_point = fetch.min_filter == xe::TextureFilter::kPoint;
  const bool mip_point = fetch.mip_filter == xe::TextureFilter::kPoint || base_map;
  d.magFilter = mag_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.minFilter = min_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.mipmapMode = mip_point ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
  if (base_map) {
    d.maxLOD = 0.0f;
  }
  u32 aniso = 0;
  switch (fetch.aniso_filter) {
  case xe::AnisoFilter::kMax_2_1:
    aniso = 2;
    break;
  case xe::AnisoFilter::kMax_4_1:
    aniso = 4;
    break;
  case xe::AnisoFilter::kMax_8_1:
    aniso = 8;
    break;
  case xe::AnisoFilter::kMax_16_1:
    aniso = 16;
    break;
  default:
    break;
  }
  const i32 forced = Settings::Anisotropy();
  if (forced == 1)
    aniso = 0;
  else if (forced > 1 && !mag_point && !min_point && mipmapped_upload)
    aniso = std::max<u32>(aniso, std::min<u32>(static_cast<u32>(forced), 16u));
  const bool aniso_on = aniso > 1 && !mag_point && !min_point;
  d.anisotropyEnabled = aniso_on;
  d.maxAnisotropy = aniso_on ? aniso : 1u;
  if (aniso_on && !base_map)
    d.mipmapMode = plume::RenderMipmapMode::LINEAR;
  d.borderColor = fetch.border_color == xe::BorderColor::k_ABGR_White
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  d.mipLODBias = static_cast<float>(fetch.lod_bias) / 32.0f;
  d.minLOD = static_cast<float>(fetch.mip_min_level);
  if (d.maxLOD != 0.0f)
    d.maxLOD = synthesized_mips ? 15.0f : static_cast<float>(fetch.mip_max_level);
  return d;
}

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc) {
  auto &s = state();
  if (!s.ready || !s.device || !s.sampler_descriptor_set)
    return kSamplerLinearClamp;

  const DescKey key{desc.addressU,    desc.addressV,      desc.addressW,
                    desc.minFilter,   desc.magFilter,     desc.mipmapMode,
                    desc.borderColor, desc.maxAnisotropy, desc.anisotropyEnabled,
                    desc.mipLODBias,  desc.minLOD,        desc.maxLOD};
  auto &c = cache();
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.second;

  u32 slot = kInvalidDescriptorIndex;
  for (size_t i = kReservedSamplerCount; i < s.sampler_descriptor_used.size(); ++i) {
    if (!s.sampler_descriptor_used[i]) {
      s.sampler_descriptor_used[i] = true;
      slot = static_cast<u32>(i);
      break;
    }
  }
  if (slot == kInvalidDescriptorIndex) {
    u32 n;
    if (DiagShouldLog(0x5A01, &n))
      EOT_WARN("[sampler-cache] bindless sampler heap full, falling back to slot 0");
    return kSamplerLinearClamp;
  }
  auto sampler = s.device->createSampler(desc);
  s.sampler_descriptor_set->setSampler(slot, sampler.get());
  c.map.emplace(key, std::make_pair(std::move(sampler), slot));
  static u32 logged = 0;
  if (logged++ < 48)
    EOT_DEBUG("[sampler] #{} slot {}: min {} mag {} mip {} aniso {}x{} lod bias {:g} lod {:g}..{:g} "
             "address {}/{}/{}",
             logged, slot, static_cast<int>(desc.minFilter), static_cast<int>(desc.magFilter),
             static_cast<int>(desc.mipmapMode), desc.anisotropyEnabled ? 1 : 0,
             desc.maxAnisotropy, desc.mipLODBias, desc.minLOD, desc.maxLOD,
             static_cast<int>(desc.addressU), static_cast<int>(desc.addressV),
             static_cast<int>(desc.addressW));
  return slot;
}

}
