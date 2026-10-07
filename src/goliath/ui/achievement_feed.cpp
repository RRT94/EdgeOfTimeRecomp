// goliath/ui/achievement_feed.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/ui/achievement_feed.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/system/achievement_manager.h>

#include "core/logging.h"

REXCVAR_DEFINE_STRING(eot_ach_toast_test, "", "EdgeOfTime/Debug", "Test achievement banner text");

namespace eot::ui {

namespace {

struct Pending {
  std::string name;
  uint32_t gamerscore = 0;
  uint32_t image_id = 0;
};

std::mutex g_mutex;
std::deque<Pending> g_queue;
constexpr size_t kMaxWaiting = 8;

constexpr uint32_t kPreviewGamerscore = 10;
constexpr uint32_t kPreviewImageId = 1;

}

void StartAchievementFeed() {
  const std::string preview = REXCVAR_GET(eot_ach_toast_test);
  if (!preview.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back({preview, kPreviewGamerscore, kPreviewImageId});
    EOT_INFO("[ach] eot_ach_toast_test: the banner will show \"{}\" once", preview);
  }
}

void QueueAchievementToast(const rex::system::AchievementEvent &event) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.size() >= kMaxWaiting)
    return;
  g_queue.push_back({event.achievement.label, event.achievement.gamerscore, event.achievement.image_id});
}

}

int32_t eot_ach_toast_take(char *name, int32_t size, int32_t *gamerscore, int32_t *image_id) {
  std::lock_guard<std::mutex> lock(eot::ui::g_mutex);
  if (eot::ui::g_queue.empty())
    return 0;
  const eot::ui::Pending next = eot::ui::g_queue.front();
  eot::ui::g_queue.pop_front();
  if (name && size > 0) {
    std::strncpy(name, next.name.c_str(), static_cast<size_t>(size) - 1);
    name[size - 1] = 0;
  }
  if (gamerscore)
    *gamerscore = static_cast<int32_t>(next.gamerscore);
  if (image_id)
    *image_id = static_cast<int32_t>(next.image_id);
  return 1;
}
