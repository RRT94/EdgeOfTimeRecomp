// goliath/controller/button_glyphs.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <string_view>

#include <rex/hook.h>

namespace eot::controller {

void ButtonGlyphsTick(const PPCContext &ctx, uint8_t *base);

bool KeyCapInstalled(uint8_t slot);
bool MenuGlyphInstalled(uint8_t slot);
void GlyphBindsChanged();
void NoteButtonHelper(uint32_t object);
void NoteButtonHelperZone(uint32_t zone);
bool BarShowsPrompts();
std::string_view ActiveGlyphSet();

}

namespace eot::goliath {

void SetWidePromptSheets(uint32_t first, uint32_t second);

void MashPromptDrawRecord(uint32_t object, uint32_t record);

}
