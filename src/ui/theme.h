// ui/theme.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    ui/theme.h
 * @brief   The chrome palette of the host ImGui overlays and the installer.
 *
 *          reblue's theme (BSD 3-Clause, Tom Clay), taken as it is: one
 *          accent fills every surface and control face, white carries every
 *          foreground mark, dimmed by alpha where a row reads as secondary.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <imgui.h>
#include <rex/ui/style.h>

namespace eot::ui {

struct Theme {
  static constexpr ImVec4 kAccent{0.078f, 0.129f, 0.259f, 1.00f};
  static constexpr ImVec4 kAccentDeep{0.043f, 0.075f, 0.161f, 1.00f};
  static constexpr ImVec4 kAccentHovered{0.129f, 0.208f, 0.392f, 1.00f};
  static constexpr ImVec4 kAccentActive{0.188f, 0.290f, 0.522f, 1.00f};
  static constexpr ImVec4 kAccentSelected{0.243f, 0.369f, 0.651f, 1.00f};
  static constexpr ImVec4 kPanel{0.043f, 0.075f, 0.161f, 0.86f};

  static constexpr ImVec4 White(float alpha) { return {1.00f, 1.00f, 1.00f, alpha}; }

  static void LoadFonts(ImFontAtlas *atlas);
  static void Apply(ImGuiStyle &style, rex::ui::Style &overlays);
};

}
