// goliath/text/glyph_pages.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace eot::text {

void NoteGlyphPackage(uint32_t package);

bool GlyphTableReady(const char *table);

bool InstallGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font_record, const char *table,
                   const char *font);

std::vector<std::string> GlyphTableLines(const char *table);

struct IconCell {
  uint8_t slot;
  float u0, v0, u1, v1;
  float aspect;
  float lift;
  uint8_t alias;
  uint8_t size_of;
};

uint32_t InstallIconPage(const PPCContext &ctx, uint8_t *base, uint32_t font_record, uint32_t page, uint32_t texture,
                         const std::vector<IconCell> &cells);

}

namespace eot::text {

bool LoadTranslation(const std::filesystem::path &game, std::string_view language);

size_t TranslatedLines();

}
