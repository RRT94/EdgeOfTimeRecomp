// goliath/ui/aspect_policy.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>
#include <rex/ppc/func.h>

namespace eot::goliath {

bool UiAspectLockActive();

float TextScaleFactor();

bool UiAspectLogEnabled();

bool HudWindowLoadsWide(uint32_t crc);

}

namespace eot::goliath {

void TitleMatteTick(const PPCContext &ctx, uint8_t *base);

}
