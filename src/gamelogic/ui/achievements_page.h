// gamelogic/ui/achievements_page.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace eot::ui::achievements {

constexpr uint32_t kActs = 3;

bool Open(const PPCContext &ctx, uint8_t *base);
void Close(const PPCContext &ctx, uint8_t *base);
bool IsOpen();

void SetAct(const PPCContext &ctx, uint8_t *base, uint32_t act);
uint32_t Act();

void Move(const PPCContext &ctx, uint8_t *base, int step);

}
