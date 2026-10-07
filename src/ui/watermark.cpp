// ui/watermark.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "ui/watermark.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/version.h>

#include "core/build_info.h"
#include "goliath/debug/freecam.h"
#include "gpu/device.h"
#include "gpu/settings.h"
#include "platform/update_check.h"

REXCVAR_DEFINE_BOOL(eot_watermark, false, "EdgeOfTime/Video", "Show the build watermark");

namespace eot::ui {

namespace {

std::string Resolution() {
  const u32 rw = gpu::InternalRenderWidth(), rh = gpu::InternalRenderHeight();
  if (!rw || !rh)
    return {};
  std::string s = std::format("{}x{}", rw, rh);
  const u32 ow = gpu::Video::OutputWidth(), oh = gpu::Video::OutputHeight();
  if (ow && oh && (ow != rw || oh != rh))
    s += std::format(" -> {}x{}", ow, oh);
  return s;
}

}

void WatermarkOverlay::OnDraw(ImGuiIO &io) {
  constexpr ImU32 kText = IM_COL32(255, 255, 255, 96);
  constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 112);
  constexpr ImU32 kNotice = IM_COL32(255, 216, 96, 232);
  constexpr ImU32 kFlying = IM_COL32(128, 224, 255, 232);

  {
    std::vector<std::string> debug_lines;
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, pitch = 0.0f;
    double speed = 0.0;
    if (debug::FreecamReadout(x, y, z, yaw, pitch, speed))
      debug_lines.push_back(
          std::format("free camera  {:.1f}, {:.1f}, {:.1f}   yaw {:.0f}  pitch {:.0f}   {:.1f}/s",
                      x, y, z, yaw, pitch, speed));
    if (debug::ScenePauseActive()) {
      const int owed = debug::ScenePauseStepsPending();
      debug_lines.push_back(owed ? std::format("scene frozen  stepping {} frame(s)", owed)
                                 : std::string("scene frozen  F9 steps a frame"));
    }
    if (std::string script = debug::InputScriptReadout(); !script.empty())
      debug_lines.push_back(std::move(script));
    ImDrawList *fdl = ImGui::GetForegroundDrawList();
    float dy = 10.0f;
    for (const std::string &line : debug_lines) {
      const ImVec2 at(10.0f, dy);
      fdl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), kShadow, line.c_str());
      fdl->AddText(at, kFlying, line.c_str());
      dy += ImGui::GetTextLineHeight();
    }
  }

  if (!REXCVAR_GET(eot_watermark))
    return;

  std::vector<std::pair<std::string, ImU32>> lines;
  if (const auto done = platform::LastInstallUpdate())
    lines.emplace_back(std::format("Updated your install to reeot {}", done->version), kNotice);

  lines.emplace_back(std::string("reeot v" REEOT_VERSION_STRING " (" REEOT_GIT_COMMIT) +
                         (REEOT_GIT_DIRTY ? "*)" : ")"),
                     kText);
  lines.emplace_back(std::string("built " REEOT_BUILD_TIMESTAMP), kText);
  lines.emplace_back(std::string("rexglue v" REXGLUE_VERSION_STRING), kText);
  if (std::string res = Resolution(); !res.empty())
    lines.emplace_back(std::move(res), kText);

  constexpr float kPad = 10.0f;
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const float line_h = ImGui::GetTextLineHeight();
  float y = io.DisplaySize.y - kPad - line_h * static_cast<float>(lines.size());
  for (const auto &[text, color] : lines) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(io.DisplaySize.x - kPad - size.x, y);
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadow, text.c_str());
    dl->AddText(pos, color, text.c_str());
    y += line_h;
  }
}

}
