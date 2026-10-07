// goliath/ui/overlays/fps.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "goliath/ui/overlays/fps.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>

#include <rex/cvar.h>

#include "core/memory_helpers.h"
#include "gpu/gpu_timing.h"

REXCVAR_DEFINE_BOOL(show_fps_overlay, false, "EdgeOfTime/Video", "Show the FPS overlay");

namespace {

using namespace eot;

std::atomic<bool> g_fps_overlay_enabled{false};

void EnsureFpsOverlayStateInitialized() {
  static const bool initialized = [] {
    g_fps_overlay_enabled.store(REXCVAR_GET(show_fps_overlay), std::memory_order_relaxed);
    rex::cvar::RegisterChangeCallback(
        "show_fps_overlay", [](std::string_view, std::string_view) {
          g_fps_overlay_enabled.store(REXCVAR_GET(show_fps_overlay),
                                      std::memory_order_release);
        });
    return true;
  }();
  (void)initialized;
}

constexpr u32 kFpsStructPointerAddr = 0x824E66D0;
constexpr u32 kFrameDeltaTimeAddr = 0x824E5A8C;
constexpr u32 kDeviceGlobalAddr = 0x82496CBC;
constexpr u32 kDeviceSwapIntervalOffset = 0x36A0;

std::optional<float> ReadEngineFps() {
  const u32 fps_struct = mem::load<u32>(kFpsStructPointerAddr);
  if (!fps_struct)
    return std::nullopt;
  const float fps = std::bit_cast<float>(mem::load<u32>(fps_struct));
  if (!std::isfinite(fps) || fps < 0.0f || fps > 10000.0f)
    return std::nullopt;
  return fps;
}

float ReadFrameDeltaMs() {
  const float dt = std::bit_cast<float>(mem::load<u32>(kFrameDeltaTimeAddr));
  return (std::isfinite(dt) && dt > 0.0f && dt < 1.0f) ? dt * 1000.0f : 0.0f;
}

std::optional<u32> ReadDeviceSwapInterval() {
  const u32 device = mem::load<u32>(kDeviceGlobalAddr);
  if (!device)
    return std::nullopt;
  return mem::load<u32>(device + kDeviceSwapIntervalOffset);
}

struct FpsRange {
  float min, max;
  ImVec4 color;
};
const FpsRange kFpsRanges[] = {
    {120.0f, 9999.0f, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)},
    {59.0f, 120.0f, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
    {30.0f, 59.0f, ImVec4(0.5f, 1.0f, 0.5f, 1.0f)},
    {20.0f, 30.0f, ImVec4(1.0f, 1.0f, 0.4f, 1.0f)},
    {10.0f, 20.0f, ImVec4(1.0f, 0.4f, 0.4f, 1.0f)},
    {0.0f, 10.0f, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)},
};
constexpr size_t kFpsRangeCount = sizeof(kFpsRanges) / sizeof(kFpsRanges[0]);

ImVec4 GetFpsColor(float fps) {
  auto lerp = [](const ImVec4 &a, const ImVec4 &b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
  };
  for (size_t i = 0; i < kFpsRangeCount; i++) {
    const FpsRange &range = kFpsRanges[i];
    if (fps >= range.min) {
      float t = (fps - range.min) / (range.max - range.min);
      if (t > 1.0f)
        t = 1.0f;
      const ImVec4 next = i > 0 ? kFpsRanges[i - 1].color : range.color;
      return lerp(range.color, next, t);
    }
  }
  return kFpsRanges[kFpsRangeCount - 1].color;
}

struct HostFrameClock {
  std::chrono::steady_clock::time_point last{};
  float samples[60] = {};
  size_t n = 0, idx = 0;
  void Tick() {
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0) {
      const float ms = std::chrono::duration<float, std::milli>(now - last).count();
      samples[idx] = ms;
      idx = (idx + 1) % 60;
      if (n < 60)
        ++n;
    }
    last = now;
  }
  float Average() const {
    if (!n)
      return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i)
      sum += samples[i];
    return sum / static_cast<float>(n);
  }
  float Worst() const {
    float worst = 0.0f;
    for (size_t i = 0; i < n; ++i)
      worst = std::max(worst, samples[i]);
    return worst;
  }
};

HostFrameClock g_host_frame_clock;

}

bool FpsOverlayEnabled() {
  EnsureFpsOverlayStateInitialized();
  return g_fps_overlay_enabled.load(std::memory_order_acquire);
}

void FpsOverlayDialog::SyncEnabledState() {
  const bool enabled = FpsOverlayEnabled();
  if (registered_ == enabled)
    return;
  g_host_frame_clock = {};
  if (enabled)
    imgui_drawer()->AddDialog(this);
  else
    imgui_drawer()->RemoveDialog(this);
  registered_ = enabled;
}

void FpsOverlayDialog::OnDraw(ImGuiIO &io) {
  (void)io;
  if (!FpsOverlayEnabled())
    return;
  if (!REX_KERNEL_STATE())
    return;
  g_host_frame_clock.Tick();

  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
  ImGui::SetNextWindowBgAlpha(0.5f);
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
  ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##fps_overlay", nullptr, flags)) {
    DrawFpsBlock();
    DrawGpuBlock();
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

void FpsOverlayDialog::DrawFpsBlock() {
  {
    const std::optional<float> fps = ReadEngineFps();
    char buf[64];
    if (fps)
      std::snprintf(buf, sizeof(buf), "FPS: %.1f", static_cast<double>(*fps));
    else
      std::snprintf(buf, sizeof(buf), "FPS: --");
    const ImVec4 color = fps ? GetFpsColor(*fps) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(color, "%s", buf);
    ImGui::SetWindowFontScale(1.0f);

    const float frame_ms = ReadFrameDeltaMs();
    const std::optional<uint32_t> interval = ReadDeviceSwapInterval();
    char diag_buf[80];
    if (interval)
      std::snprintf(diag_buf, sizeof(diag_buf), "engine %.1f ms | interval %u",
                    static_cast<double>(frame_ms), *interval);
    else
      std::snprintf(diag_buf, sizeof(diag_buf), "engine %.1f ms | interval --",
                    static_cast<double>(frame_ms));
    ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.7f, 1.0f), "%s", diag_buf);

    char host_buf[80];
    std::snprintf(host_buf, sizeof(host_buf), "host %.1f ms avg | worst %.0f ms",
                  static_cast<double>(g_host_frame_clock.Average()),
                  static_cast<double>(g_host_frame_clock.Worst()));
    ImGui::TextColored(ImVec4(0.72f, 0.86f, 1.0f, 1.0f), "%s", host_buf);
  }
}

void FpsOverlayDialog::DrawGpuBlock() {
  ImGui::Separator();
  eot::gpu::LiveStats live;
  if (!eot::gpu::ReadLiveStats(&live)) {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "gpu: collecting");
    return;
  }
  char line[128];
  std::snprintf(line, sizeof(line), "gpu %.1f ms | wall %.1f ms | %u draws | %u resolves",
                static_cast<double>(live.gpu_ms), static_cast<double>(live.wall_ms), live.draws,
                live.resolves);
  ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", line);
  std::snprintf(line, sizeof(line), "game thread capture %.1f wait %.1f | render draw %.1f (rec %.1f)",
                static_cast<double>(live.capture_ms), static_cast<double>(live.present_wait_ms),
                static_cast<double>(live.draw_ms), static_cast<double>(live.record_ms));
  ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.7f, 1.0f), "%s", line);
  for (uint32_t i = 0; i < live.row_count; ++i) {
    const eot::gpu::LiveStats::Row &row = live.rows[i];
    if (row.draws)
      std::snprintf(line, sizeof(line), "  %-16s %5.2f ms  %u draws", row.name,
                    static_cast<double>(row.ms), row.draws);
    else
      std::snprintf(line, sizeof(line), "  %-16s %5.2f ms", row.name, static_cast<double>(row.ms));
    const bool heavy = live.gpu_ms > 0.0f && row.ms >= live.gpu_ms * 0.25f;
    ImGui::TextColored(heavy ? ImVec4(1.0f, 0.75f, 0.4f, 1.0f) : ImVec4(0.72f, 0.86f, 1.0f, 1.0f),
                       "%s", line);
  }
}
