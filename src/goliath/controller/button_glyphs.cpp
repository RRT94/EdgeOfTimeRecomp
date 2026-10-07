// goliath/controller/button_glyphs.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/controller/button_glyphs.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/func.h>
#include <bit>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/mouse_input.h"
#include "goliath/controller/pad_remap.h"
#include "goliath/loading/texture_overrides.h"
#include "goliath/text/glyph_pages.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUD_RegisterGlyphTag); // (name r3, replacement r4): GLAPIHUD slot 47

namespace eot::controller {

namespace {

constexpr uint32_t kTagCount = 0x824A18FC;
constexpr uint32_t kTagNames = 0x824A1900;
constexpr uint32_t kTagText = 0x824A5900;
constexpr uint32_t kTagStride = 256;
constexpr uint32_t kTagPromptTable = 0x883C9BC8;
constexpr uint32_t kMaxTags = 64;
constexpr uint32_t kTagChars = 128;

constexpr uint32_t kRetailPage = 3;
constexpr uint32_t kFirstPage = 5;
constexpr uint32_t kRemapPage = 8;
constexpr uint32_t kKeyboardPage = 9;
constexpr uint32_t kMenuPage = 10;
constexpr uint32_t kMenuSlots = 0x22;
constexpr uint32_t kPageCount = 11;

constexpr const char *kTable = "GlyphsIcons";
constexpr const char *kSheet = "Reeot_Icons";
constexpr uint32_t kPollTicks = 30;

constexpr uint32_t kHudsDataPtr = 0x883CA288;
constexpr uint32_t kHelperOffset = 0xA4;
constexpr uint32_t kComposeFn = 0x8827C8D8;
constexpr uint32_t kZoneCount = 3;
constexpr uint32_t kHelperZones = 112;
constexpr uint32_t kHelperMasks = 124;
constexpr float kCapLift = 0.0f;

struct Box {
  float x0, y0, x1, y1;
};

struct Set {
  std::string name;
  uint32_t page;
  std::vector<eot::text::IconCell> cells;
};

struct Font {
  const char *name;
  uint32_t crc;
};
constexpr Font kFonts[] = {{"TempusGothic", eot::ui::NameCrc("TempusGothic")}, {"SansaCon", eot::ui::NameCrc("SansaCon")}};

constexpr uint8_t kMenuBackCapSlot = 0x0F;
constexpr uint8_t kMenuOptCapSlot = 0x13;

struct KeySlot {
  uint8_t slot;
  const char *cvar;
  const char *cluster;
  uint8_t size_of;
  bool retail = true;
};
constexpr KeySlot kKeySlots[] = {
    {0x00, "eot_key_jump", nullptr, 0xFF},          {0x01, "eot_key_web", nullptr, 0xFF},
    {0x02, "eot_key_heavy_attack", nullptr, 0xFF},  {0x03, "eot_key_light_attack", nullptr, 0xFF},
    {0x04, "eot_key_web_swing", nullptr, 0xFF},     {0x05, "eot_key_hyper_sense", nullptr, 0xFF},
    {0x06, nullptr, "MOUSE", 0xFF},                 {0x07, nullptr, "WASD", 0xFF},
    {0x08, "eot_key_upgrades", nullptr, 0x00},      {0x09, "eot_key_pause", nullptr, 0x00},
    {0x0A, "eot_key_grab", nullptr, 0xFF},          {0x0B, "eot_key_special_attack", nullptr, 0xFF},
    {0x1B, "keybind_lstick_down", nullptr, 0x00},   {0x1E, "eot_key_spider_sense", nullptr, 0xFF},
    {kLeftClickCapSlot, "eot_key_left_stick_click", nullptr, 0x00, false},
    {kRightClickCapSlot, "eot_key_right_stick_click", nullptr, 0x00, false},
    {kTimeParadoxCapSlot, "eot_key_time_paradox", nullptr, 0x00, false},
    {kMenuBackCapSlot, nullptr, "Escape", 0x00, false},
    {kMenuOptCapSlot, nullptr, "Delete", 0x00, false},
    {kMoveKeyCapSlots[0], kMoveKeyCvars[0], nullptr, 0x00, false},
    {kMoveKeyCapSlots[1], kMoveKeyCvars[1], nullptr, 0x00, false},
    {kMoveKeyCapSlots[2], kMoveKeyCvars[2], nullptr, 0x00, false},
    {kMoveKeyCapSlots[3], kMoveKeyCvars[3], nullptr, 0x00, false},
    {kLookKeyCapSlots[0], kLookKeyCvars[0], nullptr, 0x00, false},
    {kLookKeyCapSlots[1], kLookKeyCvars[1], nullptr, 0x00, false},
    {kLookKeyCapSlots[2], kLookKeyCvars[2], nullptr, 0x00, false},
    {kLookKeyCapSlots[3], kLookKeyCvars[3], nullptr, 0x00, false},
};

float g_sheet_w = 0, g_sheet_h = 0;
std::vector<Set> g_sets;
std::vector<std::pair<std::string, Box>> g_caps;
uint64_t g_have[kPageCount] = {};
constexpr uint32_t kSlotCount = 64;
bool g_parsed = false;

uint32_t g_texture = 0;
bool g_requested = false;
bool g_installed = false;
bool g_gave_up = false;
uint32_t g_ticks = 0;
constexpr uint32_t kMaxTicks = 12000;

uint32_t g_applied_page = kRetailPage;
uint32_t g_applied_count = 0;
std::string g_applied_setting;
PadBrand g_applied_pad = PadBrand::Unknown;
size_t g_keys_hash = 0;
size_t g_remap_hash = 0;
bool g_menu_keys = false;
std::atomic<bool> g_binds_dirty{false};
std::atomic<uint32_t> g_helper{0};
std::atomic<uint32_t> g_helper_zones{0};

Box Normalised(const Box &b) { return {b.x0 / g_sheet_w, b.y0 / g_sheet_h, b.x1 / g_sheet_w, b.y1 / g_sheet_h}; }

bool Parse() {
  const std::vector<std::string> lines = eot::text::GlyphTableLines(kTable);
  if (lines.empty())
    return false;
  size_t set = SIZE_MAX;
  bool caps = false;
  uint32_t page = kFirstPage;
  for (const std::string &line : lines) {
    char name[64];
    float a = 0, b = 0, c = 0, d = 0;
    unsigned slot = 0, size_of = 0;
    if (std::sscanf(line.c_str(), "sheet %f %f", &a, &b) == 2) {
      g_sheet_w = a;
      g_sheet_h = b;
    } else if (std::sscanf(line.c_str(), "set %63s", name) == 1) {
      if (page >= kRemapPage) {
        EOT_WARN("[glyphs] more icon sets than pages; {} dropped", name);
        set = SIZE_MAX;
        continue;
      }
      g_sets.push_back({name, page++, {}});
      set = g_sets.size() - 1;
      caps = false;
    } else if (line == "caps") {
      caps = true;
      set = SIZE_MAX;
    } else if (const int sizes = std::sscanf(line.c_str(), "cell %x %f %f %f %f %x", &slot, &a, &b, &c, &d, &size_of);
               sizes >= 5) {
      if (set == SIZE_MAX || !g_sheet_w || slot >= kSlotCount)
        continue;
      const Box uv = Normalised({a, b, c, d});
      const bool sized = sizes == 6 && size_of < kSlotCount;
      const float aspect = sized ? (c - a) / (d - b) : 0.0f;
      g_sets[set].cells.push_back({static_cast<uint8_t>(slot), uv.x0, uv.y0, uv.x1, uv.y1, aspect, 0.0f, 0xFF,
                                   static_cast<uint8_t>(sized ? size_of : 0xFFu)});
      g_have[g_sets[set].page] |= 1ull << slot;
    } else if (unsigned other = 0; std::sscanf(line.c_str(), "alias %x %x", &slot, &other) == 2) {
      if (set == SIZE_MAX || slot >= kSlotCount || other >= kSlotCount)
        continue;
      g_sets[set].cells.push_back({static_cast<uint8_t>(slot), 0, 0, 0, 0, 0.0f, 0.0f, static_cast<uint8_t>(other), 0xFF});
      g_have[g_sets[set].page] |= 1ull << slot;
    } else if (std::sscanf(line.c_str(), "cap %63s %f %f %f %f", name, &a, &b, &c, &d) == 5) {
      if (caps)
        g_caps.emplace_back(name, Box{a, b, c, d});
    }
  }
  g_parsed = g_sheet_w > 0 && !g_sets.empty();
  return g_parsed;
}

const Box *Cap(std::string_view key) {
  for (const auto &[name, box] : g_caps)
    if (name == key)
      return &box;
  return nullptr;
}

std::string BoundKey(const char *cvar) {
  std::string value = rex::cvar::GetFlagByName(cvar);
  if (const size_t comma = value.find(','); comma != std::string::npos)
    value.resize(comma);
  if (const size_t plus = value.rfind('+'); plus != std::string::npos)
    value.erase(0, plus + 1);
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(0, 1);
  return value;
}

size_t KeysHash() {
  size_t h = 1469598103934665603ull;
  for (const KeySlot &k : kKeySlots)
    if (k.cvar)
      for (const char c : BoundKey(k.cvar))
        h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return h;
}

std::vector<eot::text::IconCell> RemapCells() {
  uint8_t to[32];
  for (uint8_t s = 0; s < 32; ++s)
    to[s] = s;
  for (uint32_t i = 0; i < kPadActionCount; ++i) {
    const uint8_t native = PadInputSlot(kPadActions[i].native);
    uint8_t physical = PadInputSlot(PhysicalFor(kPadActions[i]));
    if (physical == 0xFF)
      physical = PadInputSlot(PadInput::Up);
    if (native < 32)
      to[native] = physical;
  }
  if (SticksSwapped()) {
    std::swap(to[PadInputSlot(PadInput::LS)], to[PadInputSlot(PadInput::RS)]);
  }
  std::vector<eot::text::IconCell> cells;
  g_have[kRemapPage] = 0;
  for (const KeySlot &k : kKeySlots) {
    if (!k.retail)
      continue;
    cells.push_back({k.slot, 0, 0, 0, 0, 0.0f, 0.0f, to[k.slot], 0xFF});
    g_have[kRemapPage] |= 1ull << k.slot;
  }
  return cells;
}

size_t RemapHash() {
  size_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < kPadActionCount; ++i)
    h = (h ^ static_cast<uint8_t>(PhysicalFor(kPadActions[i]))) * 1099511628211ull;
  return (h ^ (SticksSwapped() ? 1u : 0u)) * 1099511628211ull;
}

std::vector<eot::text::IconCell> KeyboardCells() {
  std::vector<eot::text::IconCell> cells;
  g_have[kKeyboardPage] = 0;
  for (const KeySlot &k : kKeySlots) {
    const Box *box = k.cluster ? Cap(k.cluster) : nullptr;
    if (!box && k.cvar) {
      const std::string key = BoundKey(k.cvar);
      box = key.empty() ? nullptr : Cap(key);
    }
    if (!box)
      continue;
    const Box uv = Normalised(*box);
    const float aspect = (box->x1 - box->x0) / (box->y1 - box->y0);
    cells.push_back({k.slot, uv.x0, uv.y0, uv.x1, uv.y1, aspect, kCapLift, 0xFF, k.size_of});
    g_have[kKeyboardPage] |= 1ull << k.slot;
  }
  return cells;
}

std::vector<eot::text::IconCell> MenuCells() {
  const Set *set = nullptr;
  for (const Set &s : g_sets)
    if (s.page == g_applied_page)
      set = &s;
  std::vector<eot::text::IconCell> cells;
  g_have[kMenuPage] = 0;
  for (uint8_t slot = 0; slot < kMenuSlots; ++slot) {
    const eot::text::IconCell *own = nullptr;
    if (set)
      for (const eot::text::IconCell &cell : set->cells)
        if (cell.slot == slot && cell.alias == 0xFF)
          own = &cell;
    if (own)
      cells.push_back(*own);
    else
      cells.push_back({slot, 0, 0, 0, 0, 0.0f, 0.0f, slot, 0xFF});
    g_have[kMenuPage] |= 1ull << slot;
  }
  return cells;
}

bool InstallInto(const PPCContext &ctx, uint8_t *base, const Font &font, const std::vector<eot::text::IconCell> &keys,
                 bool keys_only) {
  using namespace eot::loading;
  const uint32_t record = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeFont, font.crc));
  if (!record)
    return false;
  bool ok = true;
  if (!keys_only) {
    for (const Set &set : g_sets)
      ok = eot::text::InstallIconPage(ctx, base, record, set.page, g_texture, set.cells) && ok;
    ok = eot::text::InstallIconPage(ctx, base, record, kRemapPage, g_texture, RemapCells()) && ok;
    ok = eot::text::InstallIconPage(ctx, base, record, kMenuPage, g_texture, MenuCells()) && ok;
  }
  ok = eot::text::InstallIconPage(ctx, base, record, kKeyboardPage, g_texture, keys) && ok;
  ReleaseResource(ctx, base, record);
  return ok;
}

struct TagSwap {
  const char *tag;
  uint8_t retail;
  uint8_t cap;
  bool menu_layer;
};
constexpr TagSwap kTagSwaps[] = {
    {"TIMEPARADOX", 0x07, kLeftClickCapSlot, false},
    {"TIMEPARADOX", 0x06, kRightClickCapSlot, false},
    {"MENUBACK", 0x01, kMenuBackCapSlot, true},
    {"MENUCANCEL", 0x01, kMenuBackCapSlot, true},
    {"MENUOPT1", 0x03, kMenuOptCapSlot, true},
    {"NEXTHINT", 0x03, kMenuOptCapSlot, true},
};
constexpr size_t kTagSwapCount = sizeof(kTagSwaps) / sizeof(kTagSwaps[0]);

bool TagIs(uint32_t index, const char *name) {
  for (uint32_t k = 0; k < kTagChars; ++k) {
    const uint16_t c = eot::mem::load<uint16_t>(kTagNames + index * kTagStride + k * 2);
    const unsigned char want = static_cast<unsigned char>(name[k]);
    if (!want)
      return c == 0;
    if (c > 0x7F || std::toupper(static_cast<int>(c)) != std::toupper(static_cast<int>(want)))
      return false;
  }
  return false;
}

void ApplyPage(uint32_t page) {
  const uint32_t count = eot::mem::load<uint32_t>(kTagCount);
  const bool menu_layer = MenuKeysActive();
  for (uint32_t i = 0; i < count && i < kMaxTags; ++i) {
    const uint32_t text = kTagText + i * kTagStride;
    const TagSwap *swaps[kTagSwapCount];
    size_t swap_count = 0;
    for (const TagSwap &swap : kTagSwaps)
      if (TagIs(i, swap.tag))
        swaps[swap_count++] = &swap;
    for (uint32_t k = 0; k < kTagChars; ++k) {
      const uint16_t c = eot::mem::load<uint16_t>(text + k * 2);
      if (!c)
        break;
      const uint32_t high = c >> 8;
      if (high != kRetailPage && (high < kFirstPage || high > kKeyboardPage))
        continue;
      uint32_t slot = c & 0xFF;
      for (size_t n = 0; n < swap_count; ++n)
        if (slot == swaps[n]->cap)
          slot = swaps[n]->retail;
      uint32_t want = slot;
      if (page == kKeyboardPage)
        for (size_t n = 0; n < swap_count; ++n)
          if (slot == swaps[n]->retail && (!swaps[n]->menu_layer || menu_layer))
            want = swaps[n]->cap;
      const uint32_t to =
          (page != kRetailPage && want < kSlotCount && (g_have[page] & (1ull << want))) ? page : kRetailPage;
      eot::mem::store<uint16_t>(text + k * 2,
                                static_cast<uint16_t>((to << 8) | (to == kRetailPage ? slot : want)));
    }
  }
  g_applied_page = page;
  g_applied_count = count;
}

constexpr uint32_t kHelperSize = 156;

uint32_t LiveHelper() {
  const uint32_t helper = g_helper.load(std::memory_order_acquire);
  const uint32_t huds = eot::mem::load<uint32_t>(kHudsDataPtr);
  if (!helper || !eot::mem::readable(huds + kHelperOffset, 4) ||
      !eot::mem::load<uint32_t>(huds + kHelperOffset))
    return 0;
  if (!eot::mem::readable(helper, kHelperSize))
    return 0;
  for (uint32_t i = 0; i < kZoneCount; ++i) {
    const uint32_t window = eot::mem::load<uint32_t>(helper + kHelperZones + i * 4);
    if (!window || window == 0xFFFFFFFFu)
      return 0;
  }
  return helper;
}

uint32_t ZoneMask(uint32_t helper, uint32_t zone) {
  const uint32_t mask = eot::mem::load<uint32_t>(helper + kHelperMasks + zone * 4);
  for (uint32_t i = 0; i < 32; ++i)
    if ((mask & (1u << i)) && !eot::mem::load<uint32_t>(kTagPromptTable + i * 4))
      return 0;
  return mask;
}

void RecomposePrompts(const PPCContext &ctx, uint8_t *base) {
  const uint32_t helper = LiveHelper();
  PPCFunc *compose = helper ? rex::runtime::ResolveIndirectFunction(kComposeFn) : nullptr;
  if (!compose)
    return;
  const uint32_t zones = g_helper_zones.load(std::memory_order_acquire);
  for (uint32_t zone = 0; zone < kZoneCount; ++zone) {
    if (!(zones & (1u << zone)) || !ZoneMask(helper, zone))
      continue;
    PPCContext call = ctx;
    call.r3.u32 = helper;
    call.r4.u32 = zone;
    call.r5.u32 = 0;
    call.r6.u32 = 0;
    call.r7.u32 = 0;
    call.r8.u32 = 0;
    call.r9.u32 = 0;
    call.r10.u32 = 0;
    compose(call, base);
  }
}

uint32_t PageFor(std::string_view set) {
  for (const Set &s : g_sets)
    if (set == s.name)
      return s.page;
  return kRetailPage;
}

uint32_t XboxPage() { return PadRemapActive() ? kRemapPage : kRetailPage; }

uint32_t WantedPage(const std::string &setting, PadBrand pad) {
  if (setting == "keyboard")
    return kKeyboardPage;
  if (setting == "xbox")
    return XboxPage();
  if (setting != "auto")
    return PageFor(setting);
  switch (pad) {
  case PadBrand::Keyboard:
    return kKeyboardPage;
  case PadBrand::Switch:
    return PageFor("switch");
  case PadBrand::PlayStation:
    return PageFor("playstation");
  case PadBrand::SteamController:
    return PageFor("steam");
  default:
    return XboxPage();
  }
}

void RefillMenuPage(const PPCContext &ctx, uint8_t *base) {
  using namespace eot::loading;
  if (!g_texture)
    return;
  const std::vector<eot::text::IconCell> cells = MenuCells();
  for (const Font &font : kFonts) {
    const uint32_t record = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeFont, font.crc));
    if (!record)
      continue;
    eot::text::InstallIconPage(ctx, base, record, kMenuPage, g_texture, cells);
    ReleaseResource(ctx, base, record);
  }
}

}

void ButtonGlyphsTick(const PPCContext &ctx, uint8_t *base) {
  using namespace eot::loading;
  if (g_gave_up)
    return;
  if (!g_installed) {
    if (++g_ticks > kMaxTicks) {
      g_gave_up = true;
      EOT_WARN("[glyphs] the button icons never became ready; the prompts keep the Xbox art");
      return;
    }
    if (g_ticks % 8 != 0)
      return;
    if (!TextureOverridesSettled())
      return;
    if (!g_parsed && !Parse())
      return;
    if (!g_texture) {
      const uint32_t record = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeTexture, eot::ui::NameCrc(kSheet)));
      if (!record)
        return;
      if (!ResourceResident(record) || !TextureDescriptor(ctx, base, record)) {
        if (!g_requested) {
          g_requested = true;
          RequestResourceLoad(ctx, base, record);
          EOT_DEBUG("[glyphs] {} asked to load; the icon pages wait for its data", kSheet);
        }
        ReleaseResource(ctx, base, record);
        return;
      }
      g_texture = record;
    }
    g_keys_hash = KeysHash();
    g_remap_hash = RemapHash();
    g_menu_keys = MenuKeysActive();
    const std::vector<eot::text::IconCell> keys = KeyboardCells();
    for (const Font &font : kFonts)
      if (!InstallInto(ctx, base, font, keys, false))
        return;
    g_installed = true;
    EOT_INFO("[glyphs] button icons: {} set(s) and {} key cap(s) on {}x{}, pages {}..{} and {} of both fonts",
             g_sets.size(), g_caps.size(), g_sheet_w, g_sheet_h, kFirstPage, kKeyboardPage, kMenuPage);
  }

  if (g_applied_setting == "auto") {
    const PadBrand pad = ActivePad();
    if (pad != g_applied_pad) {
      g_applied_pad = pad;
      const uint32_t page = WantedPage(g_applied_setting, pad);
      if (page != g_applied_page) {
        ApplyPage(page);
        RefillMenuPage(ctx, base);
        RecomposePrompts(ctx, base);
        EOT_DEBUG("[glyphs] prompts draw page {} (auto: {})", page, ToString(pad));
      }
    }
  }

  if (const bool menu = MenuKeysActive(); menu != g_menu_keys) {
    g_menu_keys = menu;
    if (g_applied_page == kKeyboardPage) {
      ApplyPage(kKeyboardPage);
      RecomposePrompts(ctx, base);
    }
  }

  if (++g_ticks % kPollTicks == 0 || g_binds_dirty.exchange(false, std::memory_order_acq_rel)) {
    const size_t hash = KeysHash();
    if (hash != g_keys_hash) {
      g_keys_hash = hash;
      const std::vector<eot::text::IconCell> keys = KeyboardCells();
      for (const Font &font : kFonts)
        InstallInto(ctx, base, font, keys, true);
      EOT_DEBUG("[glyphs] key caps refilled for the binds as they are now");
      if (g_applied_page == kKeyboardPage)
        ApplyPage(kKeyboardPage);
    }
    const size_t remap = RemapHash();
    bool remap_moved = false;
    if (remap != g_remap_hash) {
      g_remap_hash = remap;
      remap_moved = true;
      const std::vector<eot::text::IconCell> cells = RemapCells();
      for (const Font &font : kFonts) {
        const uint32_t record = AcquireResource(ctx, base, FindResourceFromCrc(ctx, base, kTypeFont, font.crc));
        if (!record)
          continue;
        eot::text::InstallIconPage(ctx, base, record, kRemapPage, g_texture, cells);
        ReleaseResource(ctx, base, record);
      }
      EOT_DEBUG("[glyphs] the Xbox page refilled for the pad binds as they are now");
    }
    const std::string setting = rex::cvar::GetFlagByName("eot_button_glyphs");
    const PadBrand pad = setting == "auto" ? ActivePad() : PadBrand::Unknown;
    if (setting != g_applied_setting || pad != g_applied_pad || remap_moved) {
      g_applied_setting = setting;
      g_applied_pad = pad;
      const uint32_t page = WantedPage(setting, pad);
      ApplyPage(page);
      RefillMenuPage(ctx, base);
      RecomposePrompts(ctx, base);
      EOT_DEBUG("[glyphs] prompts draw page {} ({}{}{})", page, setting, setting == "auto" ? ": " : "",
                setting == "auto" ? ToString(pad) : "");
      return;
    }
  }
  if (eot::mem::load<uint32_t>(kTagCount) != g_applied_count)
    ApplyPage(g_applied_page);
}

void NoteButtonHelper(uint32_t object) {
  g_helper.store(object, std::memory_order_release);
  g_helper_zones.store(0, std::memory_order_release);
}

bool BarShowsPrompts() {
  const uint32_t helper = LiveHelper();
  if (!helper)
    return false;
  const uint32_t zones = g_helper_zones.load(std::memory_order_acquire);
  for (uint32_t zone = 0; zone < kZoneCount; ++zone)
    if ((zones & (1u << zone)) && eot::mem::load<uint32_t>(helper + kHelperMasks + zone * 4))
      return true;
  return false;
}

void NoteButtonHelperZone(uint32_t zone) {
  if (zone < kZoneCount)
    g_helper_zones.fetch_or(1u << zone, std::memory_order_acq_rel);
}

std::string_view ActiveGlyphSet() {
  if (!g_installed)
    return "xbox";
  if (g_applied_page == kKeyboardPage)
    return "keyboard";
  for (const Set &s : g_sets)
    if (s.page == g_applied_page)
      return s.name;
  return "xbox";
}

bool KeyCapInstalled(uint8_t slot) {
  return g_installed && slot < kSlotCount && (g_have[kKeyboardPage] & (1ull << slot)) != 0;
}

bool MenuGlyphInstalled(uint8_t slot) {
  return g_installed && slot < kSlotCount && (g_have[kMenuPage] & (1ull << slot)) != 0;
}

void GlyphBindsChanged() { g_binds_dirty.store(true, std::memory_order_release); }

}

REX_HOOK_RAW(eot_HUD_RegisterGlyphTag) {
  __imp__eot_HUD_RegisterGlyphTag(ctx, base);
  using namespace eot::controller;
  if (g_installed && g_applied_page != kRetailPage)
    ApplyPage(g_applied_page);
}

namespace {

constexpr uint32_t kRenderParams = 340;
constexpr uint32_t kGeometry = 48;
constexpr uint32_t kGeometryCrc = 4;
constexpr uint32_t kRecordWorld = 92;
constexpr float kWidth = 3.0f;
constexpr float kHeight = 0.75f;

struct Model {
  uint32_t sheet;
  uint32_t model;
};
constexpr uint32_t kSheetB = 0xE30428C2;
constexpr uint32_t kSheetY = 0xD047D170;
constexpr Model kModels[] = {
    {kSheetB, 0x20E1499F},
    {kSheetB, 0xC45FF3A4},
    {kSheetY, 0x6158BC05},
};

std::atomic<uint32_t> g_sheets[2] = {0, 0};

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

void ScaleRow(uint32_t world, uint32_t row, float s) {
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t at = world + (row * 4 + k) * 4;
    StoreF(at, LoadF(at) * s);
  }
}

}

namespace eot::goliath {

void SetWidePromptSheets(uint32_t first, uint32_t second) {
  g_sheets[0].store(first, std::memory_order_relaxed);
  g_sheets[1].store(second, std::memory_order_relaxed);
}

void MashPromptDrawRecord(uint32_t object, uint32_t record) {
  const uint32_t first = g_sheets[0].load(std::memory_order_relaxed);
  const uint32_t second = g_sheets[1].load(std::memory_order_relaxed);
  if ((!first && !second) || !object || !record)
    return;
  const uint32_t params = eot::mem::load<uint32_t>(object + kRenderParams);
  const uint32_t geometry = params ? eot::mem::load<uint32_t>(params + kGeometry) : 0;
  const uint32_t crc = geometry ? eot::mem::load<uint32_t>(geometry + kGeometryCrc) : 0;
  if (!crc)
    return;
  for (const Model &m : kModels) {
    if (m.model != crc || (m.sheet != first && m.sheet != second))
      continue;
    ScaleRow(record + kRecordWorld, 0, kWidth);
    ScaleRow(record + kRecordWorld, 1, kHeight);
    return;
  }
}

}
