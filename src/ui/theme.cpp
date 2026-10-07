// ui/theme.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "ui/theme.h"

#include "embedded.h"

namespace eot::ui {

void Theme::LoadFonts(ImFontAtlas *atlas) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  constexpr auto kFont = eot::Embedded("fonts/HelveticaNeueRoman.otf");
  atlas->AddFontFromMemoryTTF(const_cast<uint8_t *>(kFont.data), static_cast<int>(kFont.size), 18.0f, &cfg);
}

void Theme::Apply(ImGuiStyle &style, rex::ui::Style &overlays) {
  style.WindowRounding = 0.0f;
  style.ChildRounding = 4.0f;
  style.PopupRounding = 4.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 4.0f;
  style.ScrollbarRounding = 4.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;

  ImVec4 *c = style.Colors;
  c[ImGuiCol_Text] = White(1.00f);
  c[ImGuiCol_TextDisabled] = White(0.42f);
  c[ImGuiCol_WindowBg] = kAccent;
  c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_PopupBg] = ImVec4(kAccentDeep.x, kAccentDeep.y, kAccentDeep.z, 0.98f);
  c[ImGuiCol_Border] = White(0.28f);
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_FrameBg] = White(0.08f);
  c[ImGuiCol_FrameBgHovered] = White(0.14f);
  c[ImGuiCol_FrameBgActive] = White(0.20f);
  c[ImGuiCol_TitleBg] = kAccentDeep;
  c[ImGuiCol_TitleBgActive] = kAccent;
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(kAccentDeep.x, kAccentDeep.y, kAccentDeep.z, 0.75f);
  c[ImGuiCol_MenuBarBg] = kAccentDeep;
  c[ImGuiCol_ScrollbarBg] = White(0.06f);
  c[ImGuiCol_ScrollbarGrab] = White(0.24f);
  c[ImGuiCol_ScrollbarGrabHovered] = White(0.38f);
  c[ImGuiCol_ScrollbarGrabActive] = White(0.52f);
  c[ImGuiCol_CheckMark] = White(1.00f);
  c[ImGuiCol_SliderGrab] = White(0.70f);
  c[ImGuiCol_SliderGrabActive] = White(1.00f);
  c[ImGuiCol_Button] = kAccent;
  c[ImGuiCol_ButtonHovered] = kAccentHovered;
  c[ImGuiCol_ButtonActive] = kAccentActive;
  c[ImGuiCol_Header] = kAccentHovered;
  c[ImGuiCol_HeaderHovered] = kAccentActive;
  c[ImGuiCol_HeaderActive] = kAccentSelected;
  c[ImGuiCol_Separator] = White(0.22f);
  c[ImGuiCol_SeparatorHovered] = White(0.45f);
  c[ImGuiCol_SeparatorActive] = White(0.70f);
  c[ImGuiCol_ResizeGrip] = White(0.20f);
  c[ImGuiCol_ResizeGripHovered] = White(0.45f);
  c[ImGuiCol_ResizeGripActive] = White(0.70f);
  c[ImGuiCol_InputTextCursor] = White(1.00f);
  c[ImGuiCol_Tab] = kAccentDeep;
  c[ImGuiCol_TabHovered] = kAccentHovered;
  c[ImGuiCol_TabSelected] = kAccentActive;
  c[ImGuiCol_TabSelectedOverline] = White(0.85f);
  c[ImGuiCol_TabDimmed] = kAccentDeep;
  c[ImGuiCol_TabDimmedSelected] = kAccent;
  c[ImGuiCol_TabDimmedSelectedOverline] = White(0.35f);
  c[ImGuiCol_PlotLines] = White(0.90f);
  c[ImGuiCol_PlotLinesHovered] = White(1.00f);
  c[ImGuiCol_PlotHistogram] = White(0.75f);
  c[ImGuiCol_PlotHistogramHovered] = White(1.00f);
  c[ImGuiCol_TableHeaderBg] = kAccentHovered;
  c[ImGuiCol_TableBorderStrong] = White(0.30f);
  c[ImGuiCol_TableBorderLight] = White(0.16f);
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = White(0.04f);
  c[ImGuiCol_TextLink] = White(0.85f);
  c[ImGuiCol_TextSelectedBg] = White(0.22f);
  c[ImGuiCol_TreeLines] = White(0.25f);
  c[ImGuiCol_DragDropTarget] = White(0.90f);
  c[ImGuiCol_DragDropTargetBg] = White(0.15f);
  c[ImGuiCol_UnsavedMarker] = White(0.80f);
  c[ImGuiCol_NavCursor] = White(0.80f);
  c[ImGuiCol_NavWindowingHighlight] = White(0.70f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);

  overlays.toast.text = White(1.00f);
  overlays.toast.title = White(1.00f);
  overlays.achievements.header_text = White(0.90f);
}

}
