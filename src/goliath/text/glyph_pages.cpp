// goliath/text/glyph_pages.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/text/glyph_pages.h"

#include <bit>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

namespace eot::text {
namespace {

constexpr uint32_t kFontPages = 80;
constexpr uint32_t kFontPageCount = 88;
constexpr uint32_t kPageRecords = 256;
constexpr uint32_t kRecordSize = 40;
constexpr uint32_t kPageBytes = kPageRecords * kRecordSize;
constexpr uint32_t kRecTexture = 0;
constexpr uint32_t kRecU0 = 8;
constexpr uint32_t kRecV0 = 12;
constexpr uint32_t kRecU1 = 16;
constexpr uint32_t kRecV1 = 20;
constexpr uint32_t kRecAdvance = 24;
constexpr uint32_t kRecHeight = 28;
constexpr uint32_t kRecTop = 32;
constexpr uint32_t kRecFlags = 36;
constexpr uint32_t kFlagLive = 0x8000;
constexpr uint32_t kReference = 0x41;

constexpr uint32_t kPackageTables = 476;
constexpr uint32_t kPackageTableCount = 480;
constexpr uint32_t kTableRecordSize = 24;
constexpr uint32_t kTableEntries = 0;
constexpr uint32_t kTableText = 8;
constexpr uint32_t kTableEntryCount = 12;
constexpr uint32_t kTableNameCrc = 20;
constexpr uint32_t kEntrySize = 12;
constexpr uint32_t kEntryLines = 8;

constexpr float kUnitsX = 640.0f;
constexpr float kUnitsY = 480.0f;

std::vector<uint32_t> g_packages;
std::vector<uint32_t> g_done;

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

std::vector<std::string> TableLines(uint32_t package, uint32_t crc) {
  std::vector<std::string> lines;
  const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
  const uint32_t count = eot::mem::load<uint32_t>(package + kPackageTableCount);
  for (uint32_t t = 0; tables && t < count; ++t) {
    const uint32_t record = tables + t * kTableRecordSize;
    if (eot::mem::load<uint32_t>(record + kTableNameCrc) != crc)
      continue;
    const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
    const uint32_t text = eot::mem::load<uint32_t>(record + kTableText);
    const uint32_t n = eot::mem::load<uint32_t>(record + kTableEntryCount);
    for (uint32_t s = 0; entries && text && s < n; ++s) {
      const uint32_t lineRecords = eot::mem::load<uint32_t>(entries + s * kEntrySize + kEntryLines);
      uint32_t at = text + 2 * eot::mem::load<uint32_t>(lineRecords);
      std::string line;
      for (uint32_t k = 0; k < 96; ++k, at += 2) {
        const uint16_t c = eot::mem::load<uint16_t>(at);
        if (!c)
          break;
        line.push_back(c < 0x80 ? static_cast<char>(c) : '?');
      }
      lines.push_back(std::move(line));
    }
    break;
  }
  return lines;
}

std::vector<std::string> TableLines(const char *table) {
  const uint32_t crc = eot::ui::NameCrc(table);
  for (const uint32_t package : g_packages) {
    std::vector<std::string> lines = TableLines(package, crc);
    if (!lines.empty())
      return lines;
  }
  return {};
}

uint32_t Page(uint32_t font, uint32_t cp) {
  if ((cp >> 8) >= eot::mem::load<uint32_t>(font + kFontPageCount))
    return 0;
  return eot::mem::load<uint32_t>(eot::mem::load<uint32_t>(font + kFontPages) + (cp >> 8) * 4);
}

}

void NoteGlyphPackage(uint32_t package) {
  if (!package)
    return;
  for (const uint32_t p : g_packages)
    if (p == package)
      return;
  g_packages.push_back(package);
}

bool GlyphTableReady(const char *table) { return !TableLines(table).empty(); }

std::vector<std::string> GlyphTableLines(const char *table) { return TableLines(table); }

uint32_t InstallIconPage(const PPCContext &ctx, uint8_t *base, uint32_t font, uint32_t page, uint32_t texture,
                         const std::vector<IconCell> &cells) {
  constexpr uint32_t kButtons = 3;
  const uint32_t pages = eot::mem::load<uint32_t>(font + kFontPages);
  const uint32_t pageCount = eot::mem::load<uint32_t>(font + kFontPageCount);
  const uint32_t buttons = Page(font, kButtons << 8);
  if (!pages || !buttons || page >= pageCount || page <= kButtons)
    return 0;
  uint32_t at = eot::mem::load<uint32_t>(pages + page * 4);
  if (!at) {
    PPCContext call = ctx;
    call.r3.u32 = kPageBytes;
    call.r4.u32 = 16;
    call.r5.u32 = 0xFFFFFFFFu;
    call.r6.u32 = 0;
    __imp__eot_MMMemoryMgr_Alloc(call, base);
    at = call.r3.u32;
    if (!at)
      return 0;
    eot::mem::store<uint32_t>(pages + page * 4, at);
  }
  for (uint32_t off = 0; off < kPageBytes; off += 4)
    eot::mem::store<uint32_t>(at + off, 0);
  for (const IconCell &cell : cells) {
    const uint32_t src = buttons + cell.slot * kRecordSize;
    if (!(eot::mem::load<uint16_t>(src + kRecFlags + 2) & kFlagLive))
      continue;
    const uint32_t rec = at + cell.slot * kRecordSize;
    if (cell.alias != 0xFF) {
      const uint32_t other = buttons + cell.alias * kRecordSize;
      if (eot::mem::load<uint16_t>(other + kRecFlags + 2) & kFlagLive)
        for (uint32_t off = 0; off < kRecordSize; off += 4)
          eot::mem::store<uint32_t>(rec + off, eot::mem::load<uint32_t>(other + off));
      continue;
    }
    uint32_t size = src;
    if (cell.size_of != 0xFF) {
      const uint32_t other = buttons + cell.size_of * kRecordSize;
      if (eot::mem::load<uint16_t>(other + kRecFlags + 2) & kFlagLive)
        size = other;
    }
    const float height = LoadF(size + kRecHeight);
    eot::mem::store<uint32_t>(rec + kRecTexture, texture);
    eot::mem::store<uint32_t>(rec + 4, 0);
    StoreF(rec + kRecU0, cell.u0);
    StoreF(rec + kRecV0, cell.v0);
    StoreF(rec + kRecU1, cell.u1);
    StoreF(rec + kRecV1, cell.v1);
    StoreF(rec + kRecAdvance, cell.aspect > 0 ? height * (kUnitsY / kUnitsX) * cell.aspect : LoadF(src + kRecAdvance));
    StoreF(rec + kRecHeight, height);
    StoreF(rec + kRecTop, LoadF(size + kRecTop) + cell.lift / kUnitsY);
    eot::mem::store<uint32_t>(rec + kRecFlags, kFlagLive);
  }
  return at;
}

bool InstallGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font, const char *table, const char *name) {
  for (const uint32_t done : g_done)
    if (done == font)
      return true;
  const std::vector<std::string> lines = TableLines(table);
  if (lines.empty()) {
    EOT_WARN("[glyphs] no {} table in any mounted package yet; {} keeps its Latin-1", table, name);
    return false;
  }
  const uint32_t pages = eot::mem::load<uint32_t>(font + kFontPages);
  const uint32_t pageCount = eot::mem::load<uint32_t>(font + kFontPageCount);
  const uint32_t page0 = Page(font, 0);
  if (!pages || !page0 || pageCount <= 4) {
    EOT_WARN("[glyphs] {}: no page table to extend ({} pages)", name, pageCount);
    return false;
  }
  if (eot::mem::load<uint32_t>(pages + 4 * 4)) {
    g_done.push_back(font);
    return true;
  }
  const uint32_t reference = page0 + kReference * kRecordSize;
  const float refTop = LoadF(reference + kRecTop);

  float width = 0, oldHeight = 0, newHeight = 0;
  uint32_t page = 0, cells = 0, aliases = 0;
  const float refRows = (LoadF(reference + kRecV1) - LoadF(reference + kRecV0));
  for (const std::string &line : lines) {
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    unsigned cp = 0, target = 0;
    if (std::sscanf(line.c_str(), "size %f %f %f", &a, &b, &c) == 3) {
      width = a;
      oldHeight = b;
      newHeight = c;
      if (width <= 0 || oldHeight <= 0 || newHeight <= 0)
        return false;
      const float scale = oldHeight / newHeight;
      const uint32_t atlas = eot::mem::load<uint32_t>(reference + kRecTexture);
      for (uint32_t p = 0; p < pageCount; ++p) {
        const uint32_t pg = eot::mem::load<uint32_t>(pages + p * 4);
        for (uint32_t i = 0; pg && i < kPageRecords; ++i) {
          const uint32_t rec = pg + i * kRecordSize;
          if (!(eot::mem::load<uint16_t>(rec + kRecFlags + 2) & kFlagLive))
            continue;
          if (eot::mem::load<uint32_t>(rec + kRecTexture) != atlas)
            continue;
          StoreF(rec + kRecV0, LoadF(rec + kRecV0) * scale);
          StoreF(rec + kRecV1, LoadF(rec + kRecV1) * scale);
        }
      }
      PPCContext call = ctx;
      call.r3.u32 = kPageBytes;
      call.r4.u32 = 16;
      call.r5.u32 = 0xFFFFFFFFu;
      call.r6.u32 = 0;
      __imp__eot_MMMemoryMgr_Alloc(call, base);
      page = call.r3.u32;
      if (!page) {
        EOT_WARN("[glyphs] {}: no guest memory for the Cyrillic page", name);
        return false;
      }
      for (uint32_t off = 0; off < kPageBytes; off += 4)
        eot::mem::store<uint32_t>(page + off, 0);
      eot::mem::store<uint32_t>(pages + 4 * 4, page);
      continue;
    }
    if (!page)
      continue;
    if (std::sscanf(line.c_str(), "cell %x %f %f %f %f %f", &cp, &a, &b, &c, &d, &e) == 6) {
      if ((cp >> 8) != 4)
        continue;
      const uint32_t rec = page + (cp & 0xFF) * kRecordSize;
      eot::mem::store<uint32_t>(rec + kRecTexture, eot::mem::load<uint32_t>(reference + kRecTexture));
      eot::mem::store<uint32_t>(rec + 4, 0);
      StoreF(rec + kRecU0, a / width);
      StoreF(rec + kRecV0, b / newHeight);
      StoreF(rec + kRecU1, c / width);
      StoreF(rec + kRecV1, d / newHeight);
      StoreF(rec + kRecAdvance, (c - a) / kUnitsX);
      StoreF(rec + kRecHeight, (d - b) / kUnitsY);
      StoreF(rec + kRecTop, refTop + (refRows * oldHeight - e) / kUnitsY);
      eot::mem::store<uint32_t>(rec + kRecFlags, kFlagLive);
      ++cells;
      continue;
    }
    if (std::sscanf(line.c_str(), "alias %x %x", &cp, &target) == 2) {
      const uint32_t from = Page(font, target);
      if ((cp >> 8) != 4 || !from)
        continue;
      const uint32_t src = from + (target & 0xFF) * kRecordSize;
      const uint32_t rec = page + (cp & 0xFF) * kRecordSize;
      for (uint32_t off = 0; off < kRecordSize; off += 4)
        eot::mem::store<uint32_t>(rec + off, eot::mem::load<uint32_t>(src + off));
      ++aliases;
    }
  }
  g_done.push_back(font);
  EOT_DEBUG("[glyphs] {}: Cyrillic page at {:#x}, {} cells and {} aliases; the sheet is {}x{} ({} rows retail)", name,
            page, cells, aliases, width, newHeight, oldHeight);
  return true;
}

}

REX_EXTERN(__imp__eot_PKPackage_FindString);
REX_EXTERN(__imp__eot_StringTable_FindSlotLine);
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

namespace eot::text {
namespace {

constexpr uint32_t kPackageId = 176;
constexpr uint32_t kEntryLineCount = 4;
constexpr uint32_t kLineSize = 12;

struct Line {
  std::u16string text;
  uint32_t guest = 0;
};

std::unordered_map<uint64_t, uint32_t> g_index;
std::vector<Line> g_lines;
uint32_t g_block = 0;
size_t g_block_bytes = 0;
bool g_block_failed = false;
std::string g_language;

std::unordered_map<uint64_t, uint32_t> g_by_text;
uint64_t g_by_text_set = 0;
constexpr uint32_t kPackageMgr = 0x824C8DE8;
constexpr uint32_t kMgrPackages = 8;
constexpr uint32_t kMaxPackageId = 4096;
constexpr uint32_t kMaxTables = 64;
constexpr uint32_t kMaxEntries = 65536;
constexpr uint32_t kMaxLineChars = 512;

bool GuestHeap(uint32_t va) { return va >= 0x10000u && va < 0xFFF00000u; }

uint64_t Key(uint32_t package, uint32_t crc, uint32_t line) {
  return (static_cast<uint64_t>(package & 0xFFF) << 40) | (static_cast<uint64_t>(crc) << 8) | (line & 0xFF);
}

uint64_t HashGuestLine(uint32_t at) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t n = 0; n < kMaxLineChars; ++n, at += 2) {
    const uint16_t c = eot::mem::load<uint16_t>(at);
    if (!c)
      break;
    h = (h ^ c) * 1099511628211ull;
  }
  return h;
}

void BuildByText() {
  uint64_t set = 1469598103934665603ull;
  uint32_t loaded[kMaxPackageId];
  uint32_t n = 0;
  for (uint32_t id = 0; id < kMaxPackageId; ++id) {
    const uint32_t package = eot::mem::load<uint32_t>(kPackageMgr + kMgrPackages + id * 4);
    if (!GuestHeap(package))
      continue;
    loaded[n++] = package;
    set = (set ^ package) * 1099511628211ull;
  }
  if (set == g_by_text_set)
    return;
  g_by_text_set = set;
  g_by_text.clear();
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t package = loaded[k];
    const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
    const uint32_t count = eot::mem::load<uint32_t>(package + kPackageTableCount);
    if (!GuestHeap(tables) || count > kMaxTables)
      continue;
    const uint32_t id = eot::mem::load<uint32_t>(package + kPackageId);
    for (uint32_t t = 0; t < count; ++t) {
      const uint32_t record = tables + t * kTableRecordSize;
      const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
      const uint32_t text = eot::mem::load<uint32_t>(record + kTableText);
      const uint32_t entryCount = eot::mem::load<uint32_t>(record + kTableEntryCount);
      if (!GuestHeap(entries) || !GuestHeap(text) || entryCount > kMaxEntries)
        continue;
      for (uint32_t s = 0; s < entryCount; ++s) {
        const uint32_t entry = entries + s * kEntrySize;
        const uint32_t crc = eot::mem::load<uint32_t>(entry);
        const uint32_t lineCount = eot::mem::load<uint32_t>(entry + kEntryLineCount);
        const uint32_t lineRecords = eot::mem::load<uint32_t>(entry + kEntryLines);
        if (!GuestHeap(lineRecords) || lineCount > 256)
          continue;
        for (uint32_t l = 0; l < lineCount; ++l) {
          const auto it = g_index.find(Key(id, crc, l));
          if (it == g_index.end())
            continue;
          const uint32_t at = text + 2 * eot::mem::load<uint32_t>(lineRecords + l * kLineSize);
          if (GuestHeap(at))
            g_by_text.emplace(HashGuestLine(at), it->second);
        }
      }
    }
  }
  EOT_DEBUG("[text] {} lines of the loaded packages' tables matched to the translation, for the stream subtitles",
            g_by_text.size());
}

std::u16string Decode(std::string_view escaped) {
  std::string utf8;
  utf8.reserve(escaped.size());
  for (size_t i = 0; i < escaped.size(); ++i) {
    const char c = escaped[i];
    if (c == '\\' && i + 1 < escaped.size()) {
      const char n = escaped[++i];
      utf8.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n);
    } else {
      utf8.push_back(c);
    }
  }
  std::u16string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = lead;
    size_t length = 1;
    if (lead >= 0xF0) {
      cp = lead & 0x07;
      length = 4;
    } else if (lead >= 0xE0) {
      cp = lead & 0x0F;
      length = 3;
    } else if (lead >= 0xC0) {
      cp = lead & 0x1F;
      length = 2;
    }
    if (i + length > utf8.size())
      break;
    for (size_t k = 1; k < length; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    i += length;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

void CopyIntoGuest(PPCContext &ctx, uint8_t *base) {
  if (g_block || g_block_failed)
    return;
  size_t bytes = 0;
  for (const Line &line : g_lines)
    bytes += (line.text.size() + 1) * 2;
  PPCContext call = ctx;
  call.r3.u32 = static_cast<uint32_t>(bytes);
  call.r4.u32 = 16;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  __imp__eot_MMMemoryMgr_Alloc(call, base);
  g_block = call.r3.u32;
  if (!g_block) {
    g_block_failed = true;
    EOT_WARN("[text] no guest memory for the {} translation ({} bytes); the game's own text stands", g_language,
             bytes);
    return;
  }
  uint32_t at = g_block;
  for (Line &line : g_lines) {
    line.guest = at;
    for (const char16_t c : line.text) {
      eot::mem::store<uint16_t>(at, static_cast<uint16_t>(c));
      at += 2;
    }
    eot::mem::store<uint16_t>(at, 0);
    at += 2;
  }
  g_block_bytes = bytes;
  EOT_INFO("[text] {} translation: {} lines in the guest at {:#x} ({} bytes)", g_language, g_lines.size(), g_block,
           bytes);
}

}

bool LoadTranslation(const std::filesystem::path &game, std::string_view language) {
  g_index.clear();
  g_lines.clear();
  g_language.assign(language);
  const std::filesystem::path file = game / "Data" / "custom" / "lang" / (std::string(language) + ".tsv");
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return false;
  std::string row;
  size_t rows = 0, bad = 0;
  while (std::getline(in, row)) {
    if (!row.empty() && row.back() == '\r')
      row.pop_back();
    if (row.empty() || row[0] == '#')
      continue;
    std::vector<std::string_view> cols;
    size_t from = 0;
    for (size_t i = 0; i < 8; ++i) {
      const size_t tab = row.find('\t', from);
      if (tab == std::string::npos)
        break;
      cols.push_back(std::string_view(row).substr(from, tab - from));
      from = tab + 1;
    }
    if (cols.size() != 8) {
      ++bad;
      continue;
    }
    const uint32_t package = static_cast<uint32_t>(std::strtoul(std::string(cols[0]).c_str(), nullptr, 10));
    const uint32_t line = static_cast<uint32_t>(std::strtoul(std::string(cols[4]).c_str(), nullptr, 10));
    const uint32_t crc = static_cast<uint32_t>(std::strtoul(std::string(cols[5]).c_str(), nullptr, 16));
    g_index[Key(package, crc, line)] = static_cast<uint32_t>(g_lines.size());
    g_lines.push_back({Decode(std::string_view(row).substr(from)), 0});
    ++rows;
  }
  if (bad)
    EOT_WARN("[text] {}: {} malformed row(s) skipped", file.string(), bad);
  EOT_INFO("[text] {} translation: {} lines from {}", language, rows, file.string());
  return rows > 0;
}

size_t TranslatedLines() { return g_lines.size(); }

}

REX_HOOK_RAW(eot_PKPackage_FindString) {
  using namespace eot::text;
  const uint32_t package = ctx.r3.u32;
  const uint32_t table = ctx.r4.u32;
  const uint32_t string = ctx.r5.u32;
  const uint32_t line = ctx.r6.u32;
  if (g_index.empty()) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  if (!package || g_block_failed) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
  if (!tables || table >= eot::mem::load<uint32_t>(package + kPackageTableCount)) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t record = tables + table * kTableRecordSize;
  const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
  if (!entries || string >= eot::mem::load<uint32_t>(record + kTableEntryCount)) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t crc = eot::mem::load<uint32_t>(entries + string * kEntrySize);
  const uint32_t id = eot::mem::load<uint32_t>(package + kPackageId);
  const auto it = g_index.find(Key(id, crc, line));
  if (it == g_index.end()) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  if (!g_lines[it->second].guest)
    CopyIntoGuest(ctx, base);
  const uint32_t guest = g_lines[it->second].guest;
  if (!guest) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  ctx.r3.u32 = guest;
}

REX_HOOK_RAW(eot_StringTable_FindSlotLine) {
  using namespace eot::text;
  __imp__eot_StringTable_FindSlotLine(ctx, base);
  const uint32_t english = ctx.r3.u32;
  if (!english || g_block_failed || g_index.empty())
    return;
  if (!eot::mem::load<uint16_t>(english))
    return;
  BuildByText();
  const auto it = g_by_text.find(HashGuestLine(english));
  if (it == g_by_text.end())
    return;
  if (!g_lines[it->second].guest)
    CopyIntoGuest(ctx, base);
  if (const uint32_t guest = g_lines[it->second].guest)
    ctx.r3.u32 = guest;
}
