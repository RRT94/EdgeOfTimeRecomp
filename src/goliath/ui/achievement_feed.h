// goliath/ui/achievement_feed.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

#include "core/export.h"

namespace rex::system {
struct AchievementEvent;
}

namespace eot::ui {

void StartAchievementFeed();

void QueueAchievementToast(const rex::system::AchievementEvent &event);

}

extern "C" {
EOT_EXPORT int32_t eot_ach_toast_take(char *name, int32_t size, int32_t *gamerscore, int32_t *image_id);
}
