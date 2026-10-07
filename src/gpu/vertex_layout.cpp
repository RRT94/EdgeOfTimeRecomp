// gpu/vertex_layout.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/vertex_layout.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/memory/utils.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/shaders/guest_shaders.h"

namespace eot::gpu {

#include <cstring>

namespace {

struct LayoutCache {
  std::mutex mutex;
  std::unordered_map<u64, std::unique_ptr<InputLayout>> map;
};

LayoutCache &cache() {
  static LayoutCache c;
  return c;
}

struct DeclCacheEntry {
  u32 count = 0;
  u64 hash = 0;
  u8 raw[32 * sizeof(DeclElement)] = {};
};
std::unordered_map<u32, DeclCacheEntry> &decl_cache() {
  static std::unordered_map<u32, DeclCacheEntry> c;
  return c;
}

struct DecodedElement {
  u32 stream;
  u32 offset;
  u32 type;
  u32 usage;
  u32 usageIndex;
};

bool DecodeElements(const DeclElement *elements, u32 count, std::vector<DecodedElement> &out,
                    u64 *hash) {
  if (!elements || count == 0 || count > 32)
    return false;
  DeclElement canon[32];
  std::memcpy(canon, elements, count * sizeof(DeclElement));
  CanonicalizeDeclElements(canon, count);
  elements = canon;
  out.clear();
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const DeclElement &e = elements[i];
    DecodedElement d;
    d.stream = e.stream;
    d.offset = e.offset;
    d.type = e.type;
    d.usage = e.usage;
    d.usageIndex = e.usageIndex;
    if (d.stream == 0xFF)
      break;
    out.push_back(d);
  }
  *hash = XXH3_64bits(elements, count * sizeof(DeclElement));
  return !out.empty();
}

bool ReadDeclaration(u32 decl_va, std::vector<DecodedElement> &out, u64 *hash) {
  const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
  if (count == 0 || count > 32)
    return false;
  return DecodeElements(mem::at<DeclElement>(decl_va + obj::kDeclElements), count, out, hash);
}

const InputLayout *FindLayout(u64 key) {
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  return it == c.map.end() ? nullptr : it->second.get();
}

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count);

void SetSwapBit(InputLayout &l, u32 usage, u32 index) {
  const u32 bit = 1u << (index & 31);
  switch (static_cast<DeclUsage>(usage)) {
  case DeclUsage::TexCoord:
    l.swappedTexcoords |= bit;
    break;
  case DeclUsage::Normal:
    l.swappedNormals |= bit;
    break;
  case DeclUsage::Binormal:
    l.swappedBinormals |= bit;
    break;
  case DeclUsage::Tangent:
    l.swappedTangents |= bit;
    break;
  case DeclUsage::BlendWeight:
    l.swappedBlendWeights |= bit;
    break;
  case DeclUsage::Position:
    l.swappedPositions |= bit;
    break;
  default:
    break;
  }
}

}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok);

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va) {
  if (!declaration_va || vs.isPixel)
    return nullptr;
  const u8 *decl_bytes = mem::at<u8>(declaration_va);
  const u32 raw_count =
      decl_bytes ? rex::memory::load_and_swap<u32>(decl_bytes + obj::kDeclElementCount) : 0;
  const auto *raw_elements =
      decl_bytes ? reinterpret_cast<const DeclElement *>(decl_bytes + obj::kDeclElements) : nullptr;
  const bool raw_ok = raw_count != 0 && raw_count <= 32 && raw_elements != nullptr;
  static_assert(sizeof(vs.lastDeclRaw) >= 32 * sizeof(DeclElement));
  if (raw_ok && vs.lastLayout && vs.lastDeclVa == declaration_va &&
      vs.lastDeclCount == raw_count &&
      std::memcmp(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement)) == 0)
    return vs.lastLayout;
  const InputLayout *layout = GetInputLayoutSlow(s, vs, declaration_va, raw_count, raw_elements, raw_ok);
  if (layout && raw_ok) {
    vs.lastDeclVa = declaration_va;
    vs.lastDeclCount = raw_count;
    vs.lastLayout = layout;
    std::memcpy(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement));
  }
  return layout;
}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok) {
  (void)s;
  if (raw_ok) {
    auto dit = decl_cache().find(declaration_va);
    if (dit != decl_cache().end() && dit->second.count == raw_count &&
        std::memcmp(dit->second.raw, raw_elements, raw_count * sizeof(DeclElement)) == 0) {
      if (const InputLayout *hit = FindLayout(dit->second.hash ^ (vs.hash * 0x9E3779B97F4A7C15ull)))
        return hit;
    }
  }
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  if (!ReadDeclaration(declaration_va, decl, &decl_hash)) {
    u32 n;
    if (DiagShouldLog(0x5D00 ^ declaration_va, &n))
      EOT_WARN("[layout] declaration {:#x} unreadable", declaration_va);
    return nullptr;
  }
  if (raw_ok) {
    DeclCacheEntry &e = decl_cache()[declaration_va];
    e.count = raw_count;
    e.hash = decl_hash;
    std::memcpy(e.raw, raw_elements, raw_count * sizeof(DeclElement));
  }
  const u64 key = decl_hash ^ (vs.hash * 0x9E3779B97F4A7C15ull);
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  return BuildInputLayout(vs.hash, vs.inputs, decl, key, raw_elements, raw_count);
}

const InputLayout *GetInputLayoutFromRaw(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                         const u8 *decl_raw, u32 decl_count) {
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  const auto *elements = reinterpret_cast<const DeclElement *>(decl_raw);
  if (!DecodeElements(elements, decl_count, decl, &decl_hash))
    return nullptr;
  const u64 key = decl_hash ^ (vs_hash * 0x9E3779B97F4A7C15ull);
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  return BuildInputLayout(vs_hash, inputs, decl, key, elements, decl_count);
}

namespace {

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count) {
  auto layout = std::make_unique<InputLayout>();
  layout->key = key;
  u32 location = 0;
  bool ok = true;
  for (const VertexInput &in : inputs) {
    const DecodedElement *match = nullptr;
    for (const auto &d : decl) {
      if (d.usage == in.usage && d.usageIndex == in.usageIndex) {
        match = &d;
        break;
      }
    }
    const char *semantic = DeclUsageSemantic(in.usage);
    if (!match) {
      layout->elements.emplace_back(semantic, in.usageIndex, location++,
                                    plume::RenderFormat::R32G32B32A32_FLOAT,
                                    kSyntheticVertexSlot, 0);
      layout->needsSyntheticSlot = true;
      continue;
    }
    DeclTypeInfo t = DecodeDeclType(match->type);
    if (t.format == plume::RenderFormat::UNKNOWN) {
      EOT_WARN("[layout] vs {:016x}: {}{} decl type {:#x} (xenos fmt {}) has no host format",
               vs_hash, semantic, in.usageIndex, match->type, t.xenosFormat);
      ok = false;
      break;
    }
    if (in.usage == static_cast<u32>(DeclUsage::BlendIndices)) {
      if (t.format == plume::RenderFormat::R8G8B8A8_UNORM ||
          t.format == plume::RenderFormat::B8G8R8A8_UNORM)
        t.format = plume::RenderFormat::R8G8B8A8_UINT;
    }
    if (in.usage == static_cast<u32>(DeclUsage::TexCoord) && t.integer && t.sixteenBit) {
      if (g_mvk) {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_SINT
                                   : plume::RenderFormat::R16G16B16A16_SINT;
      } else {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_UINT
                                   : plume::RenderFormat::R16G16B16A16_UINT;
        layout->sintTexcoords |= 1u << in.usageIndex;
      }
    }
    if (g_mvk && (t.packedDec3 || t.packed111110))
      t.format = plume::RenderFormat::R32_FLOAT;
    if (t.sixteenBit)
      SetSwapBit(*layout, in.usage, in.usageIndex);
    if (t.packedDec3) {
      switch (static_cast<DeclUsage>(in.usage)) {
      case DeclUsage::Normal:
        layout->packedDec3 |= 1u << in.usageIndex;
        break;
      case DeclUsage::Tangent:
        layout->packedDec3 |= 1u << (8 + in.usageIndex);
        break;
      case DeclUsage::Binormal:
        layout->packedDec3 |= 1u << (16 + in.usageIndex);
        break;
      default:
        break;
      }
      layout->anyPacked111110 = true;
    }
    if (t.packed111110)
      layout->anyPacked111110 = true;
    if (match->stream >= 16) {
      EOT_WARN("[layout] vs {:016x}: stream {} out of range", vs_hash, match->stream);
      ok = false;
      break;
    }
    layout->elements.emplace_back(semantic, in.usageIndex, location++, t.format, match->stream,
                                  match->offset);
    layout->streamMask |= 1u << match->stream;
    if (in.usage == static_cast<u32>(DeclUsage::Position) && in.usageIndex == 1)
      layout->morphStreams |= 1u << match->stream;
    const u32 extent = match->offset + t.byteSize;
    if (extent > layout->streamExtent[match->stream])
      layout->streamExtent[match->stream] = extent;
  }
  if (!ok)
    return nullptr;
  if (layout->anyPacked111110)
    layout->spec |= kSpecR11G11B10Normal;
  if (layout->sintTexcoords)
    layout->spec |= kSpecSintTexcoord;
  if (raw && count) {
    layout->declRaw.assign(reinterpret_cast<const u8 *>(raw),
                           reinterpret_cast<const u8 *>(raw) + count * sizeof(DeclElement));
    CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(layout->declRaw.data()), count);
  }
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();
  auto *result = layout.get();
  c.map.emplace(key, std::move(layout));
  return result;
}

}

void CanonicalizeDeclElements(DeclElement *elements, u32 count) {
  for (u32 i = 0; i < count; ++i)
    elements[i].pad = 0;
}

}
