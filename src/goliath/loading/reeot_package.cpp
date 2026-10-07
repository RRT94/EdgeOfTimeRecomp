// goliath/loading/reeot_package.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/loading/texture_overrides.h"
#include "goliath/text/glyph_pages.h"
#include "goliath/ui/menu_handles.h"
#include "mods/mod_manager.h"
#include "platform/user_dirs.h"

REX_EXTERN(__imp__eot_GEEngineMgr_LoadMainPackage);
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);
REX_EXTERN(__imp__eot_PKPackageMgrBC_Load);
REX_EXTERN(__imp__eot_PKPackage_Mount); // PKPackage_Mount(package r3)
REX_EXTERN(__imp__eot_Stream_Open);
REX_EXTERN(__imp__eot_XContentCreateEx);

namespace {

constexpr uint32_t kPackageMgr = 0x824C8DE8;
constexpr uint32_t kMgrRecords = 16392;
constexpr uint32_t kMgrPackages = 8;
constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kRecName = 0;
constexpr uint32_t kRecNameSize = 128;
constexpr uint32_t kRecDepCount = 128;
constexpr uint32_t kRecDeps = 132;
constexpr uint32_t kRecFlags = 148;

uint32_t GuestAlloc(const PPCContext &ctx, uint8_t *base, uint32_t size) {
  PPCContext call = ctx;
  call.r3.u32 = size;
  call.r4.u32 = 16;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  __imp__eot_MMMemoryMgr_Alloc(call, base);
  return call.r3.u32;
}

uint32_t RegisterPackage(const PPCContext &ctx, uint8_t *base, uint32_t id, const char *name,
                         uint32_t dependency) {
  const uint32_t slot = kPackageMgr + kMgrRecords + id * 4;
  uint32_t record = eot::mem::load<uint32_t>(slot);
  if (record) {
    EOT_DEBUG("[pkg] package {} already registered at {:#x}", id, record);
    return record;
  }
  record = GuestAlloc(ctx, base, kRecordSize);
  if (!record) {
    EOT_WARN("[pkg] no guest memory for the {} record", name);
    return 0;
  }
  for (uint32_t i = 0; i < kRecordSize; ++i)
    eot::mem::store<uint8_t>(record + i, 0);
  const size_t len = std::strlen(name);
  for (size_t i = 0; i < len && i < kRecNameSize - 1; ++i)
    eot::mem::store<uint8_t>(record + kRecName + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
  eot::mem::store<uint32_t>(record + kRecDepCount, dependency ? 1u : 0u);
  if (dependency)
    eot::mem::store<uint16_t>(record + kRecDeps, static_cast<uint16_t>(dependency));
  eot::mem::store<uint32_t>(record + kRecFlags, 0);
  eot::mem::store<uint32_t>(slot, record);
  return record;
}

uint32_t LoadPackage(const PPCContext &ctx, uint8_t *base, uint32_t id) {
  PPCContext call = ctx;
  call.r3.u32 = kPackageMgr;
  call.r4.u32 = id;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  call.r7.u32 = 0;
  __imp__eot_PKPackageMgrBC_Load(call, base);
  return call.r3.u32;
}

bool IsPortOrModPackage(uint32_t id) {
  if (id == eot::ui::kReeotPackageId || id == eot::ui::kReeotMenuPackageId ||
      id == eot::ui::kReeotAchievementsPackageId || id == eot::ui::kReeotIconsPackageId ||
      id == eot::ui::kReeotSuitsPackageId)
    return true;
  for (const eot::mods::PackageToLoad &mod : eot::mods::PackagesToLoad())
    if (mod.id == id)
      return true;
  return false;
}

std::string GuestString(uint32_t addr, uint32_t max = 160) {
  std::string s;
  for (uint32_t i = 0; addr && i < max; ++i) {
    const char c = static_cast<char>(eot::mem::load<uint8_t>(addr + i));
    if (!c)
      break;
    s.push_back(c);
  }
  return s;
}

}

REX_HOOK_RAW(eot_PKPackage_Mount) {
  const uint32_t package = ctx.r3.u32;
  const uint32_t id = package ? eot::mem::load<uint32_t>(package + 176) : 0;
  const uint32_t record = id < 4096 ? eot::mem::load<uint32_t>(kPackageMgr + kMgrRecords + id * 4) : 0;
  __imp__eot_PKPackage_Mount(ctx, base);
  const std::string name = GuestString(record);
  const uint32_t mount_flags = package ? eot::mem::load<uint32_t>(package + 160) : 0u;
  const uint32_t open_flags = package ? eot::mem::load<uint32_t>(package + 164) : 0u;
  if (name.rfind("GDLC", 0) == 0)
    EOT_DEBUG("[dlc] mount id {:#x} '{}' -> flags {:#x} openFlags {:#x}", id, name, mount_flags, open_flags);
  else
    EOT_DEBUG("[pkg] mount id {} '{}' -> flags {:#x} openFlags {:#x}", id, name, mount_flags, open_flags);
  if (!IsPortOrModPackage(id)) {
    eot::loading::TextureOverridesPackageMounted(ctx, base);
    return;
  }
  if (package) {
    const uint32_t flags = eot::mem::load<uint32_t>(package + 160);
    eot::mem::store<uint32_t>(package + 160, flags | 0x8);
    EOT_DEBUG("[pkg] {} activation requested (flags {:#x} -> {:#x})", GuestString(record), flags, flags | 0x8);
  }
  eot::text::NoteGlyphPackage(package);
  eot::loading::ApplyTextureOverrides(ctx, base);
}

REX_HOOK_RAW(eot_Stream_Open) {
  const std::string name = GuestString(ctx.r4.u32);
  const uint32_t mode = ctx.r5.u32;
  __imp__eot_Stream_Open(ctx, base);
  EOT_DEBUG("[pkg] open '{}' mode {:#x} -> {:#x}", name, mode, ctx.r3.u32);
}

REX_HOOK_RAW(eot_GEEngineMgr_LoadMainPackage) {
  __imp__eot_GEEngineMgr_LoadMainPackage(ctx, base);
  using namespace eot::ui;
  struct Package {
    uint32_t id;
    std::string name;
    std::string language;
    bool mod;
  };
  std::vector<Package> packages = {
      {kReeotPackageId, kReeotPackageName, "", false},
      {kReeotMenuPackageId, kReeotMenuPackageName, "", false},
      {kReeotAchievementsPackageId, kReeotAchievementsPackageName, "", false},
      {kReeotIconsPackageId, kReeotIconsPackageName, "", false},
      {kReeotSuitsPackageId, kReeotSuitsPackageName, "", false},
  };
  for (const eot::mods::PackageToLoad &mod : eot::mods::PackagesToLoad())
    packages.push_back({mod.id, mod.name, mod.language, true});
  const std::string language = eot::platform::TranslationTag(rex::cvar::GetFlagByName("eot_language"));
  for (const Package &p : packages) {
    if (!p.language.empty() && language != p.language)
      continue;
    if (eot::mem::load<uint32_t>(kPackageMgr + kMgrPackages + p.id * 4)) {
      EOT_DEBUG("[pkg] {} (id {:#x}) is already loaded", p.name, p.id);
      continue;
    }
    if (p.mod && eot::mem::load<uint32_t>(kPackageMgr + kMgrRecords + p.id * 4)) {
      EOT_WARN("[pkg] {}: package id {:#x} is one the game's own records hold; the mod is not loaded", p.name,
               p.id);
      continue;
    }
    if (!RegisterPackage(ctx, base, p.id, p.name.c_str(), kReeotPackageDependency))
      continue;
    const uint32_t package = LoadPackage(ctx, base, p.id);
    EOT_DEBUG("[pkg] queued {} as package id {:#x}: object {:#x}, handles {:#010x}+", p.name, p.id, package,
              p.id << 20);
  }
}

namespace {

// The root name lives on the caller's stack, and an overlapped create reads it later.
constexpr uint32_t kRootNameSlots = 16;
constexpr uint32_t kRootNameSize = 64;
std::mutex g_root_names_mutex;
uint32_t g_root_names = 0;
std::atomic<uint32_t> g_root_next{0};

uint32_t RootNameSlot(const PPCContext &ctx, uint8_t *base) {
  std::lock_guard<std::mutex> lock(g_root_names_mutex);
  if (!g_root_names)
    g_root_names = GuestAlloc(ctx, base, kRootNameSlots * kRootNameSize);
  if (!g_root_names)
    return 0;
  return g_root_names + (g_root_next.fetch_add(1, std::memory_order_relaxed) % kRootNameSlots) * kRootNameSize;
}

}

REX_HOOK_RAW(eot_XContentCreateEx) {
  const uint32_t name = ctx.r4.u32;
  if (name) {
    if (const uint32_t slot = RootNameSlot(ctx, base)) {
      const std::string copy = GuestString(name, kRootNameSize - 1);
      for (size_t i = 0; i < copy.size(); ++i)
        eot::mem::store<uint8_t>(slot + static_cast<uint32_t>(i), static_cast<uint8_t>(copy[i]));
      eot::mem::store<uint8_t>(slot + static_cast<uint32_t>(copy.size()), 0);
      ctx.r4.u32 = slot;
    }
  }
  __imp__eot_XContentCreateEx(ctx, base);
}

// The game's GetFileAttributesExA(name, level, info), answered from the runtime's file system.
REX_HOOK_RAW(eot_GetFileAttributesExA) {
  const std::string path = GuestString(ctx.r3.u32, 1024);
  const uint32_t info = ctx.r5.u32;
  rex::Runtime *runtime = rex::Runtime::instance();
  rex::filesystem::VirtualFileSystem *vfs = runtime ? runtime->file_system() : nullptr;
  rex::filesystem::Entry *entry = vfs && !path.empty() ? vfs->ResolvePath(path) : nullptr;
  if (!entry || !info) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t words[9] = {
      entry->attributes(),
      static_cast<uint32_t>(entry->create_timestamp()),
      static_cast<uint32_t>(entry->create_timestamp() >> 32),
      static_cast<uint32_t>(entry->access_timestamp()),
      static_cast<uint32_t>(entry->access_timestamp() >> 32),
      static_cast<uint32_t>(entry->write_timestamp()),
      static_cast<uint32_t>(entry->write_timestamp() >> 32),
      static_cast<uint32_t>(static_cast<uint64_t>(entry->size()) >> 32),
      static_cast<uint32_t>(entry->size()),
  };
  for (uint32_t i = 0; i < 9; ++i)
    eot::mem::store<uint32_t>(info + 4 * i, words[i]);
  ctx.r3.u64 = 1;
}
