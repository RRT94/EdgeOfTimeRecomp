// goliath/ui/overlays/fps.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/ui/imgui_dialog.h>

#include "imgui.h"

bool FpsOverlayEnabled();

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
public:
  explicit FpsOverlayDialog(rex::ui::ImGuiDrawer *drawer) : rex::ui::ImGuiDialog(drawer) {}

  void SyncEnabledState();
  void OnDraw(ImGuiIO &io) override;

private:
  void DrawFpsBlock();
  void DrawGpuBlock();

  bool registered_ = true;
};
