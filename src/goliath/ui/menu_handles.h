// goliath/ui/menu_handles.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

namespace eot::ui {

inline constexpr uint32_t kReeotPackageId = 0x7EE;
inline constexpr uint32_t kReeotMenuPackageId = 0x7ED;
inline constexpr uint32_t kReeotAchievementsPackageId = 0x7EC;
inline constexpr uint32_t kReeotIconsPackageId = 0x7EA;
inline constexpr uint32_t kReeotSuitsPackageId = 0x7E9;
inline constexpr const char *kReeotPackageName = "L:/custom/ReeotUI.pak";
inline constexpr const char *kReeotMenuPackageName = "L:/custom/ReeotMenu.pak";
inline constexpr const char *kReeotAchievementsPackageName = "L:/custom/ReeotAchievements.pak";
inline constexpr const char *kReeotIconsPackageName = "L:/custom/ReeotIcons.pak";
inline constexpr const char *kReeotSuitsPackageName = "L:/custom/ReeotSuits.pak";
inline constexpr const char *kReeotPackageFolder = "custom";
inline constexpr uint32_t kReeotPackageDependency = 2;
inline constexpr uint32_t kReeotHandleBase = kReeotPackageId << 20;

inline constexpr uint32_t kHandleExitGame = kReeotHandleBase | 0;
inline constexpr uint32_t kHandleExitTitle = kReeotHandleBase | 1;
inline constexpr uint32_t kHandleExitBody = kReeotHandleBase | 2;
inline constexpr uint32_t kHandleExitToMenu = kReeotHandleBase | 3;
inline constexpr uint32_t kHandleExitTicker = kReeotHandleBase | 4;
inline constexpr uint32_t kHandleLeaveTitle = kReeotHandleBase | 5;
inline constexpr uint32_t kHandleLeaveBody = kReeotHandleBase | 6;
inline constexpr uint32_t kHandleYes = kReeotHandleBase | 7;
inline constexpr uint32_t kHandleNo = kReeotHandleBase | 8;

}
