// gamelogic/ui/options_pages.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/quit_client.h"

#include "gamelogic/ui/menu_common.h"
#include "gamelogic/ui/hud_api.h"
#include "goliath/controller/pad_actions.h"
#include "goliath/ui/name_crc.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_HUDOptionsScreen_BuildBar);         // (this r3)
REX_EXTERN(__imp__eot_HUDOptionsScreen_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnShow);           // (config r3, shown r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnUpdate);         // (config r3, dt f1) -> wants to close
REX_EXTERN(__imp__eot_WindowComponent_Teardown);          // (component r3)
REX_EXTERN(__imp__eot_TCRWindow_OpeningEnter);            // (this r3): starts the grow tween
REX_EXTERN(__imp__eot_Input_IsPressed);                   // (pad mask r3, input r4) -> pressed this frame
REX_EXTERN(__imp__eot_Input_GetAxis);                     // (pad mask r3, input r4) -> f1
REX_EXTERN(__imp__eot_MultiValueControl_Create);   // (control r3, const uint32 handles[2] r4)
REX_EXTERN(__imp__eot_MultiValueControl_SetState);
REX_EXTERN(__imp__eot_MultiValueControl_SetLabel); // (control r3, string handle r4)
REX_EXTERN(__imp__eot_SliderControl_Create);       // (control r3)
REX_EXTERN(__imp__eot_SliderControl_Show);         // (control r3, shown r4)
REX_EXTERN(__imp__eot_SliderControl_SetState);     // (control r3, selected r4)
REX_EXTERN(__imp__eot_TextWnd_SetStringHandle);    // (const uint32 *window r3, string handle r4)
REX_EXTERN(__imp__eot_Wnd_SetX);                   // (const uint32 *window r3, x f1)
REX_EXTERN(__imp__eot_Wnd_SetColorBytes);
REX_EXTERN(__imp__eot_Audio_SetFxVolume);
REX_EXTERN(__imp__eot_Audio_SetMusicVolume);
REX_EXTERN(__imp__eot_Audio_SetVoiceVolume);
REX_EXTERN(__imp__eot_Subtitles_SetEnabled); // (shown r3)

REXCVAR_DEFINE_STRING(eot_button_glyphs, "auto", "EdgeOfTime/Input", "Which button prompts show")
    .allowed({"auto", "xbox", "playstation", "switch", "steam", "keyboard"});

namespace {

using namespace eot::ui;

const PPCContext *g_ctx = nullptr;
uint8_t *g_base = nullptr;

struct CallScope {
  CallScope(const PPCContext &ctx, uint8_t *base) {
    g_ctx = &ctx;
    g_base = base;
  }
};

namespace guest {
constexpr uint32_t kFxVolume = 0x883C16FC;
constexpr uint32_t kMusicVolume = 0x883C1700;
constexpr uint32_t kVoiceVolume = 0x883C1704;
constexpr float kVolumeCeiling = 0.93f;
constexpr uint32_t kSubtitlesHider = 0x883C2194;
constexpr uint32_t kVibration = 0x883C1691;
constexpr uint32_t kInputApiPtr = 0x883CA224;
constexpr uint32_t kInputApiSetVibration = 60;
constexpr uint32_t kInvertY = 0x883CA0F5;
constexpr uint32_t kInvertX = 0x883CA0F6;
constexpr uint32_t kInvertYDefault = 0x883CA0F4;
}

double Number(std::string_view text) {
  const std::string tmp(text);
  return std::strtod(tmp.c_str(), nullptr);
}

bool Truthy(std::string_view text) { return text == "true" || text == "1"; }

std::string BoolText(bool on) { return on ? "true" : "false"; }

std::string GetVolume(uint32_t addr) {
  const float have = std::min(std::bit_cast<float>(eot::mem::load<uint32_t>(addr)), guest::kVolumeCeiling);
  char text[16];
  std::snprintf(text, sizeof(text), "%.2f", have / guest::kVolumeCeiling);
  return text;
}

using GuestCall = void (*)(PPCContext &, uint8_t *);

bool SetVolume(GuestCall setter, std::string_view text) {
  if (!g_ctx)
    return false;
  const double volume = std::clamp(Number(text), 0.0, 1.0);
  PPCContext call = *g_ctx;
  call.f1.f64 = volume * guest::kVolumeCeiling;
  setter(call, g_base);
  return true;
}

std::string GetFxVolume() { return GetVolume(guest::kFxVolume); }
std::string GetMusicVolume() { return GetVolume(guest::kMusicVolume); }
std::string GetVoiceVolume() { return GetVolume(guest::kVoiceVolume); }
bool SetFxVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetFxVolume(c, b); }, v);
}
bool SetMusicVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetMusicVolume(c, b); }, v);
}
bool SetVoiceVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetVoiceVolume(c, b); }, v);
}

std::string GetSubtitles() { return BoolText(eot::mem::load<uint32_t>(guest::kSubtitlesHider) == 0xFFFFFFFFu); }
bool SetSubtitles(std::string_view v) {
  if (!g_ctx)
    return false;
  PPCContext call = *g_ctx;
  call.r3.u32 = Truthy(v) ? 1 : 0;
  __imp__eot_Subtitles_SetEnabled(call, g_base);
  return true;
}

std::string GetVibration() { return BoolText(eot::mem::load<uint8_t>(guest::kVibration) != 0); }
bool SetVibration(std::string_view v) {
  if (!g_ctx)
    return false;
  const uint32_t on = Truthy(v) ? 1 : 0;
  eot::mem::store<uint8_t>(guest::kVibration, static_cast<uint8_t>(on));
  const uint32_t api = eot::mem::load<uint32_t>(guest::kInputApiPtr);
  if (api)
    hud::CallAt(*g_ctx, g_base, eot::mem::load<uint32_t>(api + guest::kInputApiSetVibration), on);
  return true;
}

std::string GetInvertY() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertY) != 0); }
std::string GetInvertX() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertX) != 0); }
bool SetInvertY(std::string_view v) {
  eot::mem::store<uint8_t>(guest::kInvertY, Truthy(v) ? 1 : 0);
  return true;
}
bool SetInvertX(std::string_view v) {
  eot::mem::store<uint8_t>(guest::kInvertX, Truthy(v) ? 1 : 0);
  return true;
}
std::string InvertYDefault() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertYDefault) != 0); }

std::string GetKeyboardMouse() { return BoolText(rex::cvar::Query<bool>("mnk_mode")); }
bool SetKeyboardMouse(std::string_view v) {
  const std::string on = BoolText(Truthy(v));
  return rex::cvar::SetFlagByName("mnk_mode", on) && rex::cvar::SetFlagByName("mnk_mouse", on);
}

struct Accessor {
  std::string (*get)();
  bool (*set)(std::string_view value);
  const char *reset = nullptr;
  std::string (*reset_from)() = nullptr;
};

constexpr Accessor kFxVolume = {GetFxVolume, SetFxVolume, "1"};
constexpr Accessor kMusicVolume = {GetMusicVolume, SetMusicVolume, "1"};
constexpr Accessor kVoiceVolume = {GetVoiceVolume, SetVoiceVolume, "1"};
constexpr Accessor kSubtitles = {GetSubtitles, SetSubtitles, "true"};
constexpr Accessor kVibration = {GetVibration, SetVibration, "true"};
constexpr Accessor kInvertY = {GetInvertY, SetInvertY, nullptr, InvertYDefault};
constexpr Accessor kInvertX = {GetInvertX, SetInvertX, "false"};
constexpr Accessor kKeyboardMouse = {GetKeyboardMouse, SetKeyboardMouse, "false"};

struct Choice {
  const char *text;
  const char *value;
  const char *literal = nullptr;
};

enum class Format { kPlain, kPercent, kSignedPercent, kFrameRate };

struct Slider {
  double min = 0.0, max = 1.0, step = 0.1;
  Format format = Format::kPlain;
};

enum class Opens : uint8_t {
  kNothing,
  kBinds,
  kBrightness,
};

struct Setting {
  const char *label;
  const char *description;
  const char *cvar = nullptr;
  const Accessor *accessor = nullptr;
  std::span<const Choice> choices;
  Slider slider;
  bool numeric = false;
  bool restart = false;
  bool (*enabled)() = nullptr;
  const char *disabled_text = nullptr;
  std::span<const Choice> (*choices_of)() = nullptr;
  Opens opens = Opens::kNothing;

  bool IsButton() const { return opens != Opens::kNothing; }
  bool IsSlider() const { return !IsButton() && choices.empty() && !choices_of; }
};

std::span<const Choice> Choices(const Setting &s) { return s.choices_of ? s.choices_of() : s.choices; }

std::string Value(const Setting &s) {
  if (s.IsButton())
    return {};
  return s.accessor ? s.accessor->get() : rex::cvar::GetFlagByName(s.cvar);
}

bool SetValue(const Setting &s, std::string_view value) {
  if (s.IsButton())
    return false;
  return s.accessor ? s.accessor->set(value) : rex::cvar::SetFlagByName(s.cvar, value);
}

void ResetValue(const Setting &s) {
  if (s.IsButton())
    return;
  if (!s.accessor) {
    rex::cvar::ResetToDefault(s.cvar);
    return;
  }
  if (s.accessor->reset_from)
    s.accessor->set(s.accessor->reset_from());
  else if (s.accessor->reset)
    s.accessor->set(s.accessor->reset);
}

const char *Name(const Setting &s) { return s.cvar ? s.cvar : s.label; }

struct Layout {
  const char *panel;
  const char *row_prefix;
  const char *scroll_up;
  const char *scroll_down;
  const char *info_title;
  const char *info_text;
  const char *info_value;
  const char *info_note;
  float box[4];
  float value_x, value_w;
  float arrow_gap;
};

struct Page {
  const char *title;
  const char *label;
  std::span<const Setting> settings;
  const Layout *layout;
  bool table = false;
  bool binds = false;
};

constexpr Layout kWide = {"Reeot_OptionsPanel",
                          "Reeot_OptionsRow",
                          "Reeot_OptionsScrollUp",
                          "Reeot_OptionsScrollDown",
                          "Reeot_OptionsInfoTitle",
                          "Reeot_OptionsInfoText",
                          "Reeot_OptionsInfoValue",
                          "Reeot_OptionsInfoNote",
                          {0.06f, 0.20f, 0.50f, 0.60f},
                          0.670f,
                          0.170f,
                          0.008f};
constexpr Layout kNarrow = {"Reeot_ControlsPanel",
                            "Reeot_ControlsRow",
                            "Reeot_ControlsScrollUp",
                            "Reeot_ControlsScrollDown",
                            "Reeot_ControlsInfoTitle",
                            "Reeot_ControlsInfoText",
                            "Reeot_ControlsInfoValue",
                            "Reeot_ControlsInfoNote",
                            {0.06f, 0.20f, 0.34f, 0.60f},
                            0.660f,
                            0.260f,
                            0.004f};
const Layout kTable = {"Reeot_ModsPanel",
                           "Reeot_ModsRow",
                           "Reeot_ModsScrollUp",
                           "Reeot_ModsScrollDown",
                           "Reeot_ModsNoTitle",
                           "Reeot_ModsInfo",
                           "Reeot_ModsNoValue",
                           "Reeot_ModsNoNote",
                           {0.06f, 0.20f, 0.805f, 0.60f},
                           0.670f,
                           0.170f,
                           0.008f};
const Layout kBinds = {"Reeot_BindsPanel",
                       "Reeot_BindsRow",
                       "Reeot_BindsScrollUp",
                       "Reeot_BindsScrollDown",
                       "Reeot_BindsInfoTitle",
                       "Reeot_BindsInfoText",
                       "Reeot_BindsNoValue",
                       "Reeot_BindsInfoNote",
                       {0.06f, 0.20f, 0.50f, 0.60f},
                       0.670f,
                       0.170f,
                       0.008f};
constexpr uint32_t kLayoutCount = 4;
extern const Layout kTable;
extern const Layout kBinds;
uint32_t LayoutIndex(const Layout *layout) {
  return layout == &kNarrow ? 1 : layout == &kTable ? 2 : layout == &kBinds ? 3 : 0;
}

constexpr Choice kOnOff[] = {{"REEOT_VAL_OFF", "false"}, {"REEOT_VAL_ON", "true"}};
constexpr Choice kLanguages[] = {{"REEOT_VAL_AUTO", "auto"},   {"REEOT_VAL_ENGLISH", "en"}, {"REEOT_VAL_FRENCH", "fr"},
                                 {"REEOT_VAL_ITALIAN", "it"}, {"REEOT_VAL_GERMAN", "de"},  {"REEOT_VAL_SPANISH", "es"}};
std::vector<Choice> g_language_choices(std::begin(kLanguages), std::end(kLanguages));
std::vector<std::string> g_language_strings;
std::span<const Choice> LanguageChoices() { return g_language_choices; }

void RefreshLanguageChoices() {
  g_language_choices.assign(std::begin(kLanguages), std::end(kLanguages));
  g_language_strings.clear();
  char lines[1024] = {};
  if (!mods::Api().languages || mods::Api().languages(lines, sizeof(lines)) <= 0)
    return;
  std::vector<std::pair<std::string, std::string>> found;
  std::string_view rest(lines);
  while (!rest.empty()) {
    const size_t nl = rest.find('\n');
    const std::string_view line = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
    const size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0)
      continue;
    found.emplace_back(std::string(line.substr(0, tab)), std::string(line.substr(tab + 1)));
  }
  g_language_strings.reserve(found.size() * 2);
  for (const auto &[tag, name] : found) {
    bool shipped = false;
    for (const Choice &c : kLanguages)
      shipped = shipped || tag == c.value;
    if (shipped)
      continue;
    g_language_strings.push_back(tag);
    g_language_strings.push_back(name.empty() ? tag : name);
  }
  for (size_t i = 0; i + 1 < g_language_strings.size(); i += 2)
    g_language_choices.push_back({nullptr, g_language_strings[i].c_str(), g_language_strings[i + 1].c_str()});
}
constexpr Choice kNormalInverted[] = {{"REEOT_VAL_NORMAL", "false"}, {"REEOT_VAL_INVERTED", "true"}};
constexpr Choice kResolution[] = {{"REEOT_VAL_NATIVE", "native"}, {"REEOT_VAL_720P", "720p"},
                                  {"REEOT_VAL_1080P", "1080p"},   {"REEOT_VAL_1440P", "1440p"},
                                  {"REEOT_VAL_2160P", "2160p"},   {"REEOT_VAL_DISPLAY", "display"}};
constexpr Choice kFullscreenMode[] = {{nullptr, "borderless", "1"}, {nullptr, "exclusive", "2"}};
constexpr Choice kAspect[] = {{"REEOT_VAL_4_3", "4:3"},   {"REEOT_VAL_16_10", "16:10"}, {"REEOT_VAL_16_9", "16:9"},
                              {"REEOT_VAL_21_9", "21:9"}, {"REEOT_VAL_32_9", "32:9"}};
constexpr Choice kPreset[] = {{"REEOT_VAL_LOW", "low"}, {"REEOT_VAL_MEDIUM", "medium"}, {"REEOT_VAL_HIGH", "high"},
                              {"REEOT_VAL_ULTRA", "ultra"}, {"REEOT_VAL_CUSTOM", "custom"}};
constexpr Choice kMsaa[] = {{"REEOT_VAL_OFF", "0"}, {"REEOT_VAL_2X", "2"}, {"REEOT_VAL_4X", "4"}, {"REEOT_VAL_8X", "8"}};
constexpr Choice kAnisotropy[] = {{"REEOT_VAL_OFF", "0"}, {"REEOT_VAL_2X", "2"},   {"REEOT_VAL_4X", "4"},
                                  {"REEOT_VAL_8X", "8"},  {"REEOT_VAL_16X", "16"}};
constexpr Choice kShadowSize[] = {{"REEOT_VAL_AUTO", "0"},     {"REEOT_VAL_1024", "1024"},
                                  {"REEOT_VAL_2048", "2048"}, {"REEOT_VAL_4096", "4096"},
                                  {"REEOT_VAL_8192", "8192"}};
constexpr Choice kUpscale[] = {{"REEOT_VAL_BILINEAR", "bilinear"}, {"REEOT_VAL_BICUBIC", "bicubic"},
                               {"REEOT_VAL_LANCZOS", "lanczos"}};
constexpr Choice kHostAa[] = {{"REEOT_VAL_OFF", "off"}, {"REEOT_VAL_TAA", "taa"}};
constexpr Choice kGlyphs[] = {{"REEOT_VAL_AUTO", "auto"},
                              {"REEOT_VAL_XBOX", "xbox"},
                              {"REEOT_VAL_PLAYSTATION", "playstation"},
                              {"REEOT_VAL_SWITCH", "switch"},
                              {"REEOT_VAL_STEAM", "steam"},
                              {"REEOT_VAL_KEYBOARD", "keyboard"}};

bool FullscreenOn() { return rex::cvar::Query<bool>("fullscreen"); }

bool KeyboardMouseOn() { return rex::cvar::Query<bool>("mnk_mode"); }
bool MouseLookOn() { return KeyboardMouseOn() && FullscreenOn(); }

// Host AA runs on D3D12 only for now; the Vulkan builds grey it out.
bool HostAaAvailable() {
#if defined(EOT_D3D12)
  return true;
#else
  return false;
#endif
}

constexpr Setting kAudioSettings[] = {
    {.label = "REEOT_OPT_SFX_VOLUME", .description = nullptr, .accessor = &kFxVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_VOICE_VOLUME", .description = nullptr, .accessor = &kVoiceVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_MUSIC_VOLUME", .description = nullptr, .accessor = &kMusicVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_SUBTITLES", .description = "REEOT_DESC_SUBTITLES", .accessor = &kSubtitles,
     .choices = kOnOff},
    {.label = "REEOT_OPT_LANGUAGE", .description = "REEOT_DESC_LANGUAGE", .cvar = "eot_language",
     .restart = true, .choices_of = LanguageChoices},
};

constexpr Setting kVideoSettings[] = {
    {.label = "REEOT_OPT_FULLSCREEN", .description = "REEOT_DESC_FULLSCREEN", .cvar = "fullscreen",
     .choices = kOnOff},
    {.label = "REEOT_OPT_RESOLUTION", .description = "REEOT_DESC_RESOLUTION", .cvar = "eot_resolution",
     .choices = kResolution, .restart = true, .enabled = FullscreenOn, .disabled_text = "REEOT_VAL_STRETCH"},
    {.label = "REEOT_OPT_FULLSCREEN_MODE", .description = "REEOT_DESC_FULLSCREEN_MODE",
     .cvar = "eot_fullscreen_mode", .choices = kFullscreenMode, .restart = true, .enabled = FullscreenOn},
    {.label = "REEOT_OPT_RENDER_SCALE", .description = "REEOT_DESC_RENDER_SCALE", .cvar = "eot_render_scale",
     .slider = {0.5, 2.0, 0.25, Format::kPercent}, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_ASPECT", .description = "REEOT_DESC_ASPECT", .cvar = "eot_aspect_ratio",
     .choices = kAspect, .restart = true, .enabled = FullscreenOn, .disabled_text = "REEOT_VAL_STRETCH"},
    {.label = "REEOT_OPT_FOV", .description = "REEOT_DESC_FOV", .cvar = "eot_fov_scale",
     .slider = {0.7, 1.5, 0.05, Format::kPercent}, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_FPS_LIMIT", .description = "REEOT_DESC_FPS_LIMIT", .cvar = "eot_fps_limit",
     .slider = {30.0, 240.0, 10.0, Format::kFrameRate}, .numeric = true},
    {.label = "REEOT_OPT_VSYNC", .description = "REEOT_DESC_VSYNC", .cvar = "eot_vsync", .choices = kOnOff},
    {.label = "REEOT_OPT_BRIGHTNESS", .description = "REEOT_DESC_BRIGHTNESS", .opens = Opens::kBrightness},
    {.label = "REEOT_OPT_CONTRAST", .description = "REEOT_DESC_CONTRAST", .cvar = "eot_contrast",
     .slider = {0.5, 1.5, 0.02, Format::kPercent}, .numeric = true},
    {.label = "REEOT_OPT_SATURATION", .description = "REEOT_DESC_SATURATION", .cvar = "eot_saturation",
     .slider = {0.0, 2.0, 0.1, Format::kPercent}, .numeric = true},
    {.label = "REEOT_OPT_GAMMA", .description = "REEOT_DESC_GAMMA", .cvar = "eot_gamma",
     .slider = {0.5, 2.0, 0.1, Format::kPercent}, .numeric = true},
};

constexpr Setting kGraphicsSettings[] = {
    {.label = "REEOT_OPT_PRESET", .description = "REEOT_DESC_PRESET", .cvar = "eot_quality_preset",
     .choices = kPreset},
    {.label = "REEOT_OPT_MSAA", .description = "REEOT_DESC_MSAA", .cvar = "eot_msaa", .choices = kMsaa,
     .numeric = true, .restart = true},
    {.label = "REEOT_OPT_HOST_AA", .description = "REEOT_DESC_HOST_AA", .cvar = "eot_host_aa",
     .choices = kHostAa, .enabled = HostAaAvailable, .disabled_text = "REEOT_VAL_OFF"},
    {.label = "REEOT_OPT_MOTION_VECTORS", .description = "REEOT_DESC_MOTION_VECTORS",
     .cvar = "eot_motion_vectors", .choices = kOnOff},
    {.label = "REEOT_OPT_ANISOTROPY", .description = "REEOT_DESC_ANISOTROPY", .cvar = "eot_anisotropy",
     .choices = kAnisotropy, .numeric = true},
    {.label = "REEOT_OPT_SHADOW_SIZE", .description = "REEOT_DESC_SHADOW_SIZE", .cvar = "eot_shadow_map_size",
     .choices = kShadowSize, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_UPSCALE", .description = "REEOT_DESC_UPSCALE", .cvar = "eot_upscale", .choices = kUpscale},
    {.label = "REEOT_OPT_BLOOM", .description = "REEOT_DESC_BLOOM", .cvar = "eot_bloom", .choices = kOnOff},
    {.label = "REEOT_OPT_DOF", .description = "REEOT_DESC_DOF", .cvar = "eot_depth_of_field",
     .choices = kOnOff},
    {.label = "REEOT_OPT_MOTION_BLUR", .description = "REEOT_DESC_MOTION_BLUR", .cvar = "eot_motion_blur",
     .choices = kOnOff},
    {.label = "REEOT_OPT_COLOR_GRADING", .description = "REEOT_DESC_COLOR_GRADING", .cvar = "eot_color_grading",
     .choices = kOnOff},
};

constexpr Setting kGameSettings[] = {
    {.label = "REEOT_OPT_VIBRATION", .description = "REEOT_DESC_VIBRATION", .accessor = &kVibration,
     .choices = kOnOff},
    {.label = "REEOT_OPT_CAMERA_Y", .description = "REEOT_DESC_CAMERA_Y", .accessor = &kInvertY,
     .choices = kNormalInverted},
    {.label = "REEOT_OPT_CAMERA_X", .description = "REEOT_DESC_CAMERA_X", .accessor = &kInvertX,
     .choices = kNormalInverted},
    {.label = "REEOT_OPT_ACHIEVEMENT_TOASTS", .description = "REEOT_DESC_ACHIEVEMENT_TOASTS",
     .cvar = "eot_achievement_notifications", .choices = kOnOff},
    {.label = "REEOT_OPT_SUITS_REMASTER", .description = "REEOT_DESC_SUITS_REMASTER",
     .cvar = "eot_suits_remaster", .choices = kOnOff},
};

constexpr Setting kControlsSettings[] = {
    {.label = "REEOT_OPT_GLYPHS", .description = "REEOT_DESC_GLYPHS", .cvar = "eot_button_glyphs",
     .choices = kGlyphs},
    {.label = "REEOT_OPT_MNK", .description = "REEOT_DESC_MNK", .accessor = &kKeyboardMouse, .choices = kOnOff},
    {.label = "REEOT_OPT_MOUSE_SENSITIVITY", .description = "REEOT_DESC_MOUSE_SENSITIVITY",
     .cvar = "mnk_sensitivity", .slider = {0.1, 3.0, 0.1, Format::kPlain}, .numeric = true,
     .enabled = MouseLookOn},
    {.label = "REEOT_OPT_BACKGROUND_INPUT", .description = "REEOT_DESC_BACKGROUND_INPUT",
     .cvar = "eot_background_input", .choices = kOnOff},
    {.label = "REEOT_OPT_CONFIGURE_BUTTONS", .description = "REEOT_DESC_CONFIGURE_BUTTONS", .opens = Opens::kBinds},
};

constexpr Page kAudioPage = {"REEOT_AUDIO_TITLE", "Audio", kAudioSettings, &kWide};
constexpr Page kVideoPage = {"REEOT_VIDEO_TITLE", "Video", kVideoSettings, &kWide};
constexpr Page kGraphicsPage = {"REEOT_GRAPHICS_TITLE", "Graphics", kGraphicsSettings, &kWide};
constexpr Page kGamePage = {"REEOT_GAME_TITLE", "Game", kGameSettings, &kWide};
constexpr Page kControlsPage = {"REEOT_CONTROLS_TITLE", "Controls", kControlsSettings, &kNarrow};

Page g_mods_page = {"REEOT_MODS_TITLE", "Mods", {}, &kTable, true};

Page g_binds_page = {"REEOT_BINDS_TITLE", "Binds", {}, &kBinds, false, true};

enum class BindKind : uint8_t {
  kAction,
  kStick
};
struct BindRow {
  const char *label;
  const char *description;
  BindKind kind;
  int8_t action;
  const char *cvar;
  uint8_t key_slot;
  uint8_t pad_slot;
  bool right_stick;
};
#define EOT_BIND_STICK(label, desc, right, d)                                                                          \
  {                                                                                                                    \
    label, desc, BindKind::kStick, -1,                                                                                  \
        (right) ? eot::controller::kLookKeyCvars[d] : eot::controller::kMoveKeyCvars[d],                               \
        (right) ? eot::controller::kLookKeyCapSlots[d] : eot::controller::kMoveKeyCapSlots[d],                         \
        (right) ? eot::controller::kRightStickDirSlots[d] : eot::controller::kLeftStickDirSlots[d], (right)            \
  }
constexpr BindRow kBindRows[] = {
    {"REEOT_BIND_JUMP", "REEOT_DESC_BIND_JUMP", BindKind::kAction, 0},
    {"REEOT_BIND_WEB", "REEOT_DESC_BIND_WEB", BindKind::kAction, 1},
    {"REEOT_BIND_MELEE", "REEOT_DESC_BIND_MELEE", BindKind::kAction, 2},
    {"REEOT_BIND_RANGED", "REEOT_DESC_BIND_RANGED", BindKind::kAction, 3},
    {"REEOT_BIND_GRAB", "REEOT_DESC_BIND_GRAB", BindKind::kAction, 4},
    {"REEOT_BIND_THROW", "REEOT_DESC_BIND_THROW", BindKind::kAction, 5},
    {"REEOT_BIND_WEB_SWING", "REEOT_DESC_BIND_WEB_SWING", BindKind::kAction, 6},
    {"REEOT_BIND_SPECIAL", "REEOT_DESC_BIND_SPECIAL", BindKind::kAction, 7},
    {"REEOT_BIND_SPIDER_SENSE", "REEOT_DESC_BIND_SPIDER_SENSE", BindKind::kAction, 8},
    EOT_BIND_STICK("REEOT_BIND_MOVE_UP", "REEOT_DESC_BIND_MOVE", false, 0),
    EOT_BIND_STICK("REEOT_BIND_MOVE_DOWN", "REEOT_DESC_BIND_MOVE", false, 1),
    EOT_BIND_STICK("REEOT_BIND_MOVE_LEFT", "REEOT_DESC_BIND_MOVE", false, 2),
    EOT_BIND_STICK("REEOT_BIND_MOVE_RIGHT", "REEOT_DESC_BIND_MOVE", false, 3),
    {"REEOT_BIND_WALL", "REEOT_DESC_BIND_WALL", BindKind::kAction, 9},
    EOT_BIND_STICK("REEOT_BIND_LOOK_UP", "REEOT_DESC_BIND_CAMERA", true, 0),
    EOT_BIND_STICK("REEOT_BIND_LOOK_DOWN", "REEOT_DESC_BIND_CAMERA", true, 1),
    EOT_BIND_STICK("REEOT_BIND_LOOK_LEFT", "REEOT_DESC_BIND_CAMERA", true, 2),
    EOT_BIND_STICK("REEOT_BIND_LOOK_RIGHT", "REEOT_DESC_BIND_CAMERA", true, 3),
    {"REEOT_BIND_CENTER", "REEOT_DESC_BIND_CENTER", BindKind::kAction, 10},
    {"REEOT_BIND_TIME_PARADOX", "REEOT_DESC_BIND_TIME_PARADOX", BindKind::kAction, 13},
    {"REEOT_BIND_PAUSE", "REEOT_DESC_BIND_PAUSE", BindKind::kAction, 11},
    {"REEOT_BIND_UPGRADES", "REEOT_DESC_BIND_UPGRADES", BindKind::kAction, 12},
};
constexpr uint32_t kBindRowCount = sizeof(kBindRows) / sizeof(kBindRows[0]);
static_assert(eot::controller::kPadActionCount == 14, "the rows above index kPadActions");

constexpr uint32_t kColumnKey = 0;
constexpr uint32_t kColumnPad = 1;
uint32_t g_bind_column = kColumnKey;
struct Capture {
  bool active = false;
  uint32_t row = 0;
  uint32_t column = kColumnKey;
};
Capture g_capture;
bool g_bind_fixed_note = false;
uint32_t g_bind_redraw_in = 0;
constexpr uint32_t kBindRedrawFrames = 4;
Opens g_pending_opens = Opens::kNothing;
uint32_t g_options_screen = 0;
std::vector<eot_mod_info> g_mods;
std::string g_remove_armed;
int32_t g_import_state = EOT_MODS_IDLE;
std::string g_mods_message;
bool g_mods_changed = false;
bool g_prompts_sent = false;

bool ImportIdle() { return g_import_state == EOT_MODS_IDLE; }

void RefreshMods() {
  g_mods.clear();
  const mods::Host &api = mods::Api();
  if (!api.count || !api.get)
    return;
  const int32_t count = api.count();
  for (int32_t i = 0; i < count; ++i) {
    eot_mod_info info;
    if (api.get(i, &info))
      g_mods.push_back(info);
  }
}

std::string Narrow(std::string_view utf8) {
  std::string out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      const uint32_t code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
      out.push_back(code < 0x100 ? static_cast<char>(code) : '?');
      i += 2;
    } else {
      out.push_back('?');
      i += (c & 0xF0) == 0xF0 ? 4 : (c & 0xE0) == 0xE0 ? 3 : 1;
    }
  }
  return out;
}

constexpr uint32_t kRetailCount = 5;
constexpr uint32_t kAudioIndex = 0;
constexpr uint32_t kVideoIndex = 1;
constexpr uint32_t kGraphicsIndex = 2;
constexpr uint32_t kGameIndex = 3;
constexpr uint32_t kDifficultyIndex = 4;
constexpr uint32_t kControlsIndex = 5;
constexpr uint32_t kModsIndex = 6;
constexpr uint32_t kScreenCursorOff = 84;
constexpr uint32_t kRetailBrightnessIndex = 1;
constexpr uint32_t kEventSize = 16;
uint32_t g_event = 0;
bool g_brightness_select = false;

bool ModsOnBar() { return rex::cvar::Query<bool>("eot_debug_mode"); }
uint32_t LastBarIndex() { return ModsOnBar() ? kModsIndex : kControlsIndex; }

const Page *PageAt(uint32_t cursor) {
  switch (cursor) {
  case kAudioIndex:
    return &kAudioPage;
  case kVideoIndex:
    return &kVideoPage;
  case kGraphicsIndex:
    return &kGraphicsPage;
  case kGameIndex:
    return &kGamePage;
  case kControlsIndex:
    return &kControlsPage;
  case kModsIndex:
    return ModsOnBar() ? &g_mods_page : nullptr;
  default:
    return nullptr;
  }
}

namespace cfg {
constexpr uint32_t kVtable = 0;
constexpr uint32_t kPriority = 4;
constexpr uint32_t kPadMask = 8;
constexpr uint32_t kInputDelay = 12;
constexpr uint32_t kOpenTime = 16;
constexpr uint32_t kCloseTime = 20;
constexpr uint32_t kBackdropAlpha = 24;
constexpr uint32_t kFlags = 28;
constexpr uint32_t kTitle = 32;
constexpr uint32_t kTitleCount = 6;
constexpr uint32_t kWindow = 56;
constexpr uint32_t kAuxWindow = 60;
constexpr uint32_t kAuxWindowCount = 3;
constexpr uint32_t kOne = 72;
constexpr uint32_t kPage = 76;
constexpr uint32_t kBody = 84;
constexpr uint32_t kResult = 88;
constexpr uint32_t kSize = 320;

constexpr uint8_t kFlagInputBlock = 0x04;
constexpr uint8_t kFlagOwnedByCaller = 0x20;
constexpr uint8_t kFlagBlackout = 0x40;

constexpr uint32_t kResultAcceptSave = 0;
constexpr uint32_t kResultAccept = 1;
constexpr uint32_t kResultCancel = 2;
constexpr uint32_t kResultNone = 3;

constexpr uint32_t kGameOptionsVtable = 0x880894DC;
constexpr uint32_t kYesNoVtable = 0x8808840C;
constexpr uint32_t kYesNoChoice = 100;
constexpr uint32_t kYesNoYes = 1;
}

constexpr uint32_t kComponentConfigOff = 36;
// The object that opened the window, which hears its result.
constexpr uint32_t kComponentOwnerOff = 40;
constexpr uint32_t kApiLogicPtr = 0x883CA22C;
constexpr uint32_t kMsgInputEvent = 0x0C844A2F;

constexpr uint32_t kInputAccept = 9;
constexpr uint32_t kInputBack = 10;
constexpr uint32_t kInputReset = 18;
constexpr uint32_t kInputRemove = 19;
constexpr uint32_t kInputAxisX = 7;
constexpr uint32_t kInputAxisY = 8;
constexpr uint32_t kPadMask = 1;

constexpr double kRepeatDelay = 0.40;
constexpr double kRepeatInterval = 0.09;
constexpr double kAxisHeld = 0.5;

namespace ctl {
constexpr uint32_t kRoot = 0;
constexpr uint32_t kLabel = 4;
constexpr uint32_t kChoiceLeft = 8;
constexpr uint32_t kChoiceRight = 12;
constexpr uint32_t kChoiceValue = 16;
constexpr uint32_t kChoiceHandles = 20;
constexpr uint32_t kChoiceHandleSlots = 8;
constexpr uint32_t kChoiceCount = 52;
constexpr uint32_t kChoiceSize = 56;
constexpr uint32_t kSliderKnob = 20;
constexpr uint32_t kSliderKnobMin = 24;
constexpr uint32_t kSliderKnobMax = 28;
constexpr uint32_t kSliderSize = 40;
constexpr uint32_t kStateIdle = 0;
constexpr uint32_t kStateSelected = 1;
constexpr uint32_t kStateDisabled = 2;
}

constexpr uint8_t kShadeIdle[4] = {135, 135, 135, 255};
constexpr uint8_t kShadeSelected[4] = {230, 220, 220, 255};
constexpr uint8_t kShadeDisabled[4] = {135, 135, 135, 62};

constexpr uint32_t kRows = 6;
constexpr uint32_t kLayoutBlock = kRows * (ctl::kChoiceSize + ctl::kSliderSize);
constexpr uint32_t kBlockControls = 0;
constexpr uint32_t kBlockHandles = kBlockControls + kLayoutCount * kLayoutBlock;
constexpr uint32_t kBlockColour = kBlockHandles + 16;
constexpr uint32_t kBlockText = kBlockColour + 16;
constexpr uint32_t kBlockTextSize = 640;
constexpr uint32_t kBlockLine = kBlockText + kBlockTextSize;
constexpr uint32_t kBlockLineChars = 1024;
constexpr uint32_t kBlockSize = kBlockLine + kBlockLineChars * 2;

namespace tcr {
constexpr uint32_t kTweenStart = 76;
constexpr uint32_t kTweenEnd = 92;
constexpr uint32_t kCurrentSlotPtr = 0x883CA29C;
constexpr uint32_t kSlotTable = 0x88401058;
constexpr uint32_t kSlotSize = 28;
constexpr uint32_t kTitleTextCrc = 0xAE495FEE;
constexpr float kStartScale = 0.1f;
}

constexpr float kTextScale = 1.1f;
constexpr float kHeaderScale = 0.7f;
constexpr uint32_t kTextWndSetScale = 18;
constexpr uint32_t kTextWndGetXYScale = 67;
constexpr uint32_t kInfoTitleFits = 20;

constexpr float kTextBoxY = 0.03f;
constexpr float kTextBoxH = 0.50f;
constexpr float kArrowW = 0.031f;
constexpr float kArrowY = 0.07f, kArrowH = 0.42f;

uint32_t g_config = 0;
uint32_t g_restart_config = 0;
uint32_t g_block = 0;
uint32_t g_popup_id = 0;
bool g_popup_open = false;
bool g_restart_asked = false;

const Page *g_page = nullptr;
uint32_t g_cursor = 0;
uint32_t g_first = 0;
std::vector<std::string> g_opened_with;
double g_held = 0.0;
int g_held_dir = 0;

struct Windows {
  uint32_t panel = hud::kNoWindow;
  uint32_t row[kRows] = {};
  uint32_t name[kRows] = {};
  uint32_t creator[kRows] = {};
  uint32_t kind[kRows] = {};
  uint32_t state[kRows] = {};
  uint32_t header[4] = {};
  uint32_t action[kRows] = {};
  uint32_t key[kRows] = {};
  uint32_t pad[kRows] = {};
  uint32_t footer = hud::kNoWindow;
  uint32_t scroll_up = hud::kNoWindow;
  uint32_t scroll_down = hud::kNoWindow;
  uint32_t info_title = hud::kNoWindow;
  uint32_t info_text = hud::kNoWindow;
  uint32_t info_value = hud::kNoWindow;
  uint32_t info_note = hud::kNoWindow;
  float info_title_scale = 0.0f;
  bool found = false;
  bool built = false;
};
Windows g_windows[kLayoutCount];
uint32_t g_title = hud::kNoWindow;

const Layout &CurrentLayout() { return *g_page->layout; }
Windows &CurrentWindows() { return g_windows[LayoutIndex(g_page->layout)]; }

float g_retail_tween[8] = {};
bool g_retail_tween_saved = false;

uint32_t g_video_label = 0;
uint32_t g_graphics_label = 0;
uint32_t g_mods_label = 0;

uint32_t ChoiceControl(uint32_t row) {
  return g_block + kBlockControls + LayoutIndex(g_page->layout) * kLayoutBlock + row * ctl::kChoiceSize;
}
uint32_t SliderControl(uint32_t row) {
  return g_block + kBlockControls + LayoutIndex(g_page->layout) * kLayoutBlock + kRows * ctl::kChoiceSize +
         row * ctl::kSliderSize;
}

bool Pressed(const PPCContext &ctx, uint8_t *base, uint32_t input) {
  PPCContext call = ctx;
  call.r3.u32 = kPadMask;
  call.r4.u32 = input;
  __imp__eot_Input_IsPressed(call, base);
  return (call.r3.u32 & 0xFF) != 0;
}

double Axis(const PPCContext &ctx, uint8_t *base, uint32_t input) {
  PPCContext call = ctx;
  call.r3.u32 = kPadMask;
  call.r4.u32 = input;
  __imp__eot_Input_GetAxis(call, base);
  return call.f1.f64;
}

uint32_t StringHandle(const PPCContext &ctx, uint8_t *base, const char *name) {
  const uint32_t handle = hud::FindString(ctx, base, NameCrc(name));
  if (!handle)
    EOT_WARN("[menu] no string named {}; is ReeotMenu.pkz mounted?", name);
  return handle;
}

void CallWithFloat(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3, float f1) {
  PPCFunc *fn = addr ? rex::runtime::ResolveIndirectFunction(addr) : nullptr;
  if (!fn)
    return;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.f1.f64 = f1;
  fn(call, base);
}

void ScaleText(const PPCContext &ctx, uint8_t *base, uint32_t window, float factor) {
  if (window == hud::kNoWindow || !window)
    return;
  if (factor == 1.0f) {
    CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), window, 0.0f);
    return;
  }
  const uint32_t scratch = g_block + kBlockColour;
  eot::mem::store<uint32_t>(scratch, 0);
  eot::mem::store<uint32_t>(scratch + 4, 0);
  hud::Call(ctx, base, kTextWndGetXYScale, window, scratch, scratch + 4);
  const float style = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
  if (style > 0.0f)
    CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), window, style * factor);
}

void SetRect(const PPCContext &ctx, uint8_t *base, uint32_t window, float x, float y, float w, float h) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t rect = g_block + kBlockColour;
  hud::Call(ctx, base, hud::kWndGetPos, window, rect);
  const float wanted[4] = {x, y, w, h};
  for (uint32_t i = 0; i < 4; ++i)
    if (wanted[i] >= 0.0f)
      eot::mem::store<uint32_t>(rect + i * 4, std::bit_cast<uint32_t>(wanted[i]));
  hud::Call(ctx, base, hud::kWndSetPos, window, rect);
}

void PlaceLabel(const PPCContext &ctx, uint8_t *base, uint32_t window) {
  SetRect(ctx, base, window, -1.0f, kTextBoxY, -1.0f, kTextBoxH);
}

void PlaceValue(const PPCContext &ctx, uint8_t *base, const Layout &layout, uint32_t value, uint32_t left,
                uint32_t right) {
  SetRect(ctx, base, value, layout.value_x, kTextBoxY, layout.value_w, kTextBoxH);
  SetRect(ctx, base, left, layout.value_x - kArrowW - layout.arrow_gap, kArrowY, kArrowW, kArrowH);
  SetRect(ctx, base, right, layout.value_x + layout.value_w + layout.arrow_gap, kArrowY, kArrowW, kArrowH);
}

void SetShade(const PPCContext &ctx, uint8_t *base, uint32_t window_ptr, const uint8_t rgba[4]) {
  const uint32_t colour = g_block + kBlockColour;
  for (uint32_t i = 0; i < 4; ++i)
    eot::mem::store<uint8_t>(colour + i, rgba[i]);
  PPCContext call = ctx;
  call.r3.u32 = window_ptr;
  call.r4.u32 = colour;
  __imp__eot_Wnd_SetColorBytes(call, base);
}

void ShadeWindow(const PPCContext &ctx, uint8_t *base, uint32_t window, const uint8_t rgba[4]) {
  if (window == hud::kNoWindow || !window)
    return;
  eot::mem::store<uint32_t>(g_block + kBlockHandles + 12, window);
  SetShade(ctx, base, g_block + kBlockHandles + 12, rgba);
}

void SetStringHandle(const PPCContext &ctx, uint8_t *base, uint32_t window, uint32_t string_handle) {
  if (window == hud::kNoWindow || !window || !string_handle)
    return;
  eot::mem::store<uint32_t>(g_block + kBlockHandles + 8, window);
  PPCContext call = ctx;
  call.r3.u32 = g_block + kBlockHandles + 8;
  call.r4.u32 = string_handle;
  __imp__eot_TextWnd_SetStringHandle(call, base);
}

void SetLine(const PPCContext &ctx, uint8_t *base, uint32_t window, const char *line) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t text = g_block + kBlockText;
  uint32_t n = 0;
  for (uint32_t i = 0; line[i] && n < kBlockTextSize - 2; ++i) {
    eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>(line[i]));
    if (line[i] == '%')
      eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>('%'));
  }
  eot::mem::store<uint8_t>(text + n, 0);
  hud::Call(ctx, base, hud::kTextWndSetString, window, text);
}

bool Enabled(const Setting &s) { return !s.enabled || s.enabled(); }

int CurrentChoice(const Setting &s) {
  const std::string value = Value(s);
  const std::span<const Choice> choices = Choices(s);
  for (size_t i = 0; i < choices.size(); ++i) {
    const bool same = s.numeric ? std::fabs(Number(choices[i].value) - Number(value)) < 1e-4
                                : value == choices[i].value;
    if (same)
      return static_cast<int>(i);
  }
  return -1;
}

int NearestChoice(const Setting &s) {
  if (!s.numeric)
    return 0;
  const double have = Number(Value(s));
  int best = 0;
  double best_gap = 1e300;
  const std::span<const Choice> choices = Choices(s);
  for (size_t i = 0; i < choices.size(); ++i) {
    const double gap = std::fabs(Number(choices[i].value) - have);
    if (gap < best_gap) {
      best_gap = gap;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void ShowChoiceValue(const PPCContext &ctx, uint8_t *base, uint32_t control, const Setting &s) {
  if (s.IsButton()) {
    SetLine(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceValue), "");
    return;
  }
  const int index = CurrentChoice(s);
  const Choice *choice = index >= 0 ? &Choices(s)[static_cast<size_t>(index)] : nullptr;
  const char *text = !Enabled(s) && s.disabled_text ? s.disabled_text : choice ? choice->text : nullptr;
  if (text) {
    PPCContext call = ctx;
    call.r3.u32 = control + ctl::kChoiceValue;
    call.r4.u32 = StringHandle(ctx, base, text);
    __imp__eot_TextWnd_SetStringHandle(call, base);
    return;
  }
  SetLine(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceValue),
          choice && choice->literal ? choice->literal : Value(s).c_str());
}

void BindChoiceRow(const PPCContext &ctx, uint8_t *base, uint32_t row, const Setting &s, bool selected) {
  const uint32_t control = ChoiceControl(row);
  PPCContext call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = StringHandle(ctx, base, s.label);
  __imp__eot_MultiValueControl_SetLabel(call, base);
  const std::span<const Choice> choices = Choices(s);
  hud::Activate(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceLeft), choices.size() > 1);
  hud::Activate(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceRight), choices.size() > 1);
  const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(choices.size()), ctl::kChoiceHandleSlots);
  for (uint32_t i = 0; i < count; ++i)
    eot::mem::store<uint32_t>(control + ctl::kChoiceHandles + i * 4,
                              choices[i].text ? StringHandle(ctx, base, choices[i].text) : 0);
  eot::mem::store<uint32_t>(control + ctl::kChoiceCount, count);
  ShowChoiceValue(ctx, base, control, s);
  call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = !Enabled(s) ? ctl::kStateDisabled : selected ? ctl::kStateSelected : ctl::kStateIdle;
  __imp__eot_MultiValueControl_SetState(call, base);
}

bool StepChoice(const Setting &s, int step) {
  const std::span<const Choice> choices = Choices(s);
  const int count = static_cast<int>(choices.size());
  int index = CurrentChoice(s);
  if (index < 0)
    index = NearestChoice(s) - (step > 0 ? 1 : 0);
  index = ((index + step) % count + count) % count;
  return SetValue(s, choices[static_cast<size_t>(index)].value);
}

int SliderStops(const Setting &s) {
  const int steps = static_cast<int>(std::lround((s.slider.max - s.slider.min) / s.slider.step));
  return steps + 1 + (s.slider.format == Format::kFrameRate ? 1 : 0);
}

bool IsUnlimitedStop(const Setting &s, int stop) {
  return s.slider.format == Format::kFrameRate && stop == SliderStops(s) - 1;
}

double StopValue(const Setting &s, int stop) {
  return IsUnlimitedStop(s, stop) ? 0.0 : s.slider.min + s.slider.step * stop;
}

int CurrentStop(const Setting &s) {
  const double have = Number(Value(s));
  const int stops = SliderStops(s);
  if (s.slider.format == Format::kFrameRate && have <= 0.0)
    return stops - 1;
  const int last_numbered = stops - 1 - (s.slider.format == Format::kFrameRate ? 1 : 0);
  const int stop = static_cast<int>(std::lround((have - s.slider.min) / s.slider.step));
  return std::clamp(stop, 0, last_numbered);
}

void FormatNumber(const Setting &s, double value, char *out, size_t size) {
  switch (s.slider.format) {
  case Format::kPercent:
    std::snprintf(out, size, "%d%%", static_cast<int>(std::lround(value * 100.0)));
    break;
  case Format::kSignedPercent:
    std::snprintf(out, size, "%+d%%", static_cast<int>(std::lround(value * 100.0)));
    break;
  case Format::kFrameRate:
    if (value <= 0.0)
      std::snprintf(out, size, "Unlimited");
    else
      std::snprintf(out, size, "%d", static_cast<int>(std::lround(value)));
    break;
  default:
    std::snprintf(out, size, "%g", value);
    break;
  }
}

void FormatCurrent(const Setting &s, char *out, size_t size) { FormatNumber(s, Number(Value(s)), out, size); }

void ShowSliderValue(const PPCContext &ctx, uint8_t *base, uint32_t control, const Setting &s) {
  const int stop = CurrentStop(s);
  const float t = static_cast<float>(stop) / static_cast<float>(std::max(1, SliderStops(s) - 1));
  const float lo = std::bit_cast<float>(eot::mem::load<uint32_t>(control + ctl::kSliderKnobMin));
  const float hi = std::bit_cast<float>(eot::mem::load<uint32_t>(control + ctl::kSliderKnobMax));
  PPCContext call = ctx;
  call.r3.u32 = control + ctl::kSliderKnob;
  call.f1.f64 = lo + (hi - lo) * t;
  __imp__eot_Wnd_SetX(call, base);
}

void BindSliderRow(const PPCContext &ctx, uint8_t *base, uint32_t row, const Setting &s, bool selected) {
  const uint32_t control = SliderControl(row);
  PPCContext call = ctx;
  call.r3.u32 = control + ctl::kLabel;
  call.r4.u32 = StringHandle(ctx, base, s.label);
  __imp__eot_TextWnd_SetStringHandle(call, base);
  ShowSliderValue(ctx, base, control, s);
  const bool on = Enabled(s);
  call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = on && selected ? 1 : 0;
  __imp__eot_SliderControl_SetState(call, base);
  if (!on)
    for (uint32_t off = ctl::kRoot; off <= ctl::kSliderKnob; off += 4)
      SetShade(ctx, base, control + off, kShadeDisabled);
}

bool StepSlider(const Setting &s, int step) {
  const int stop = std::clamp(CurrentStop(s) + step, 0, SliderStops(s) - 1);
  if (stop == CurrentStop(s))
    return false;
  char value[32];
  std::snprintf(value, sizeof(value), "%g", StopValue(s, stop));
  return SetValue(s, value);
}

void ShowRowKind(const PPCContext &ctx, uint8_t *base, uint32_t row, bool slider) {
  const uint32_t choice_root = eot::mem::load<uint32_t>(ChoiceControl(row) + ctl::kRoot);
  hud::Call(ctx, base, slider ? hud::kWndRemoveFlags : hud::kWndAddFlags, choice_root, hud::kFlagActive);
  PPCContext call = ctx;
  call.r3.u32 = SliderControl(row);
  call.r4.u32 = slider ? 1 : 0;
  __imp__eot_SliderControl_Show(call, base);
}

uint32_t StringLength(const PPCContext &ctx, uint8_t *base, uint32_t string_handle) {
  if (!string_handle)
    return 0;
  constexpr uint32_t kStringTableResolveHandle = 0x821813A8;
  PPCFunc *fn = rex::runtime::ResolveIndirectFunction(kStringTableResolveHandle);
  if (!fn)
    return 0;
  const uint32_t line = g_block + kBlockLine;
  eot::mem::store<uint16_t>(line, 0);
  PPCContext call = ctx;
  call.r3.u32 = line;
  call.r4.u32 = string_handle;
  call.r5.u32 = 0;
  call.r6.u32 = 0;
  call.r7.u32 = 0;
  call.r8.u32 = 0;
  call.r9.u32 = 0;
  call.r10.u32 = 0;
  fn(call, base);
  uint32_t n = 0;
  while (n < kBlockLineChars - 1 && eot::mem::load<uint16_t>(line + n * 2))
    ++n;
  return n;
}

std::string ResolveText(const PPCContext &ctx, uint8_t *base, uint32_t string_handle) {
  std::string text;
  const uint32_t n = StringLength(ctx, base, string_handle);
  const uint32_t line = g_block + kBlockLine;
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t c = eot::mem::load<uint16_t>(line + i * 2);
    text.push_back(c < 0x100 ? static_cast<char>(c) : '?');
  }
  return text;
}

void FitInfoTitle(const PPCContext &ctx, uint8_t *base, uint32_t length) {
  Windows &w = CurrentWindows();
  if (w.info_title == hud::kNoWindow)
    return;
  if (w.info_title_scale <= 0.0f) {
    const uint32_t scratch = g_block + kBlockColour;
    eot::mem::store<uint32_t>(scratch, 0);
    eot::mem::store<uint32_t>(scratch + 4, 0);
    hud::Call(ctx, base, kTextWndGetXYScale, w.info_title, scratch, scratch + 4);
    w.info_title_scale = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
    if (w.info_title_scale <= 0.0f)
      return;
  }
  const float factor =
      length > kInfoTitleFits ? static_cast<float>(kInfoTitleFits) / static_cast<float>(length) : 1.0f;
  CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), w.info_title,
                factor < 1.0f ? w.info_title_scale * factor : 0.0f);
}

void ShowInfo(const PPCContext &ctx, uint8_t *base, const Setting &s) {
  const Windows &w = CurrentWindows();
  const uint32_t label = StringHandle(ctx, base, s.label);
  FitInfoTitle(ctx, base, StringLength(ctx, base, label));
  SetStringHandle(ctx, base, w.info_title, label);
  hud::Activate(ctx, base, w.info_text, s.description != nullptr);
  if (s.description)
    SetStringHandle(ctx, base, w.info_text, StringHandle(ctx, base, s.description));
  hud::Activate(ctx, base, w.info_value, s.IsSlider());
  if (s.IsSlider()) {
    char now[32];
    FormatCurrent(s, now, sizeof(now));
    SetLine(ctx, base, w.info_value, now);
  }
  hud::Activate(ctx, base, w.info_note, s.restart);
}

void ShowModRows(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  const uint32_t count = static_cast<uint32_t>(g_mods.size());
  static const char *const kKindStrings[] = {"REEOT_KIND_PACKAGE", "REEOT_KIND_REPLACEMENT", "REEOT_KIND_MODEL"};
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = g_first + row;
    const bool used = index < count;
    for (const uint32_t *column : {w.row, w.name, w.creator, w.kind, w.state})
      hud::Activate(ctx, base, column[row], used);
    if (!used)
      continue;
    const eot_mod_info &mod = g_mods[index];
    SetLine(ctx, base, w.name[row], Narrow(mod.name).c_str());
    SetLine(ctx, base, w.creator[row], Narrow(mod.creator).c_str());
    const uint32_t kind = mod.kind >= 0 && mod.kind < 3 ? static_cast<uint32_t>(mod.kind) : 1;
    SetStringHandle(ctx, base, w.kind[row], StringHandle(ctx, base, kKindStrings[kind]));
    SetStringHandle(ctx, base, w.state[row], StringHandle(ctx, base, mod.enabled ? "REEOT_VAL_ON" : "REEOT_VAL_OFF"));
    const uint8_t *shade = index == g_cursor ? kShadeSelected : mod.enabled ? kShadeIdle : kShadeDisabled;
    for (const uint32_t *column : {w.name, w.creator, w.kind, w.state})
      ShadeWindow(ctx, base, column[row], shade);
  }
  hud::Activate(ctx, base, w.scroll_up, g_first > 0);
  hud::Activate(ctx, base, w.scroll_down, g_first + kRows < count);
  std::string footer;
  if (g_import_state == EOT_MODS_CHOOSING || g_import_state == EOT_MODS_IMPORTING)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, g_import_state == EOT_MODS_CHOOSING
                                                                ? "REEOT_MODS_CHOOSING"
                                                                : "REEOT_MODS_IMPORTING"));
  else if (!g_mods_message.empty())
    footer = Narrow(g_mods_message);
  else if (count == 0)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_MODS_EMPTY"));
  else if (g_cursor < count && !g_remove_armed.empty() && g_remove_armed == g_mods[g_cursor].folder)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_MODS_REMOVE_ARMED"));
  else if (g_cursor < count)
    footer = Narrow(g_mods[g_cursor].file) + ": " + Narrow(g_mods[g_cursor].status);
  if (g_mods_changed && g_mods_message.empty())
    footer += " " + ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_DESC_RESTART"));
  SetLine(ctx, base, w.info_text, footer.c_str());
}

void ShowBindRows(const PPCContext &ctx, uint8_t *base);

void ShowRows(const PPCContext &ctx, uint8_t *base) {
  if (g_page->table) {
    ShowModRows(ctx, base);
    return;
  }
  if (g_page->binds) {
    ShowBindRows(ctx, base);
    return;
  }
  const Windows &w = CurrentWindows();
  const uint32_t count = static_cast<uint32_t>(g_page->settings.size());
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = g_first + row;
    const bool used = index < count;
    hud::Activate(ctx, base, w.row[row], used);
    if (!used)
      continue;
    const Setting &s = g_page->settings[index];
    ShowRowKind(ctx, base, row, s.IsSlider());
    if (s.IsSlider())
      BindSliderRow(ctx, base, row, s, index == g_cursor);
    else
      BindChoiceRow(ctx, base, row, s, index == g_cursor);
  }
  hud::Activate(ctx, base, w.scroll_up, g_first > 0);
  hud::Activate(ctx, base, w.scroll_down, g_first + kRows < count);
  ShowInfo(ctx, base, g_page->settings[g_cursor]);
}

void BuildRows(const PPCContext &ctx, uint8_t *base) {
  Windows &w = CurrentWindows();
  const Layout &layout = CurrentLayout();
  if (w.built)
    return;
  if (&layout == &kTable) {
    for (uint32_t row = 0; row < kRows; ++row)
      for (const uint32_t *column : {w.name, w.creator, w.kind, w.state})
        ScaleText(ctx, base, column[row], kTextScale);
    for (uint32_t i = 0; i < 4; ++i)
      ScaleText(ctx, base, w.header[i], kHeaderScale);
    w.built = true;
    EOT_DEBUG("[menu] the mods table's {} strips sized", kRows);
    return;
  }
  if (&layout == &kBinds) {
    for (uint32_t i = 0; i < 3; ++i)
      ScaleText(ctx, base, w.header[i], kHeaderScale);
    w.built = true;
    EOT_DEBUG("[menu] the binds table's {} strips sized", kRows);
    return;
  }
  const uint32_t handles = g_block + kBlockHandles;
  eot::mem::store<uint32_t>(handles, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(handles + 4, 0xFFFFFFFFu);
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t choice = ChoiceControl(row);
    for (uint32_t off = 0; off < ctl::kChoiceSize; off += 4)
      eot::mem::store<uint32_t>(choice + off, 0);
    PPCContext call = ctx;
    call.r3.u32 = choice;
    call.r4.u32 = handles;
    __imp__eot_MultiValueControl_Create(call, base);
    uint32_t root = eot::mem::load<uint32_t>(choice + ctl::kRoot);
    hud::Call(ctx, base, hud::kWndSetParent, root, w.row[row]);
    hud::Call(ctx, base, hud::kWndAddFlags, root, hud::kFlagActive);
    PlaceLabel(ctx, base, eot::mem::load<uint32_t>(choice + ctl::kLabel));
    PlaceValue(ctx, base, layout, eot::mem::load<uint32_t>(choice + ctl::kChoiceValue),
               eot::mem::load<uint32_t>(choice + ctl::kChoiceLeft), eot::mem::load<uint32_t>(choice + ctl::kChoiceRight));
    for (uint32_t off : {ctl::kLabel, ctl::kChoiceValue})
      ScaleText(ctx, base, eot::mem::load<uint32_t>(choice + off), kTextScale);

    const uint32_t slider = SliderControl(row);
    for (uint32_t off = 0; off < ctl::kSliderSize; off += 4)
      eot::mem::store<uint32_t>(slider + off, 0);
    call = ctx;
    call.r3.u32 = slider;
    __imp__eot_SliderControl_Create(call, base);
    root = eot::mem::load<uint32_t>(slider + ctl::kRoot);
    hud::Call(ctx, base, hud::kWndSetParent, root, w.row[row]);
    PlaceLabel(ctx, base, eot::mem::load<uint32_t>(slider + ctl::kLabel));
    ScaleText(ctx, base, eot::mem::load<uint32_t>(slider + ctl::kLabel), kTextScale);
  }
  w.built = true;
  EOT_DEBUG("[menu] {} settings rows built for the {} layout", kRows, &layout == &kNarrow ? "narrow" : "wide");
}

bool FindWindows(const PPCContext &ctx, uint8_t *base) {
  Windows &w = CurrentWindows();
  const Layout &layout = CurrentLayout();
  if (w.found)
    return true;
  w.panel = hud::Find(ctx, base, NameCrc(layout.panel));
  w.scroll_up = hud::Find(ctx, base, NameCrc(layout.scroll_up));
  w.scroll_down = hud::Find(ctx, base, NameCrc(layout.scroll_down));
  w.info_title = hud::Find(ctx, base, NameCrc(layout.info_title));
  w.info_text = hud::Find(ctx, base, NameCrc(layout.info_text));
  w.info_value = hud::Find(ctx, base, NameCrc(layout.info_value));
  w.info_note = hud::Find(ctx, base, NameCrc(layout.info_note));
  if (g_title == hud::kNoWindow)
    g_title = hud::Find(ctx, base, tcr::kTitleTextCrc);
  for (uint32_t i = 0; i < kRows; ++i) {
    char name[48];
    std::snprintf(name, sizeof(name), "%s%02u", layout.row_prefix, i);
    w.row[i] = hud::Find(ctx, base, NameCrc(name));
  }
  struct Column {
    const char *prefix;
    uint32_t *slots;
    uint32_t count;
  };
  if (&layout == &kTable) {
    const Column columns[] = {{"Reeot_ModsName", w.name, kRows},
                              {"Reeot_ModsCreator", w.creator, kRows},
                              {"Reeot_ModsKind", w.kind, kRows},
                              {"Reeot_ModsState", w.state, kRows},
                              {"Reeot_ModsHead", w.header, 4}};
    for (const Column &column : columns)
      for (uint32_t i = 0; i < column.count; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "%s%02u", column.prefix, i);
        column.slots[i] = hud::Find(ctx, base, NameCrc(name));
      }
  }
  if (&layout == &kBinds) {
    const Column columns[] = {{"Reeot_BindsAction", w.action, kRows},
                              {"Reeot_BindsKey", w.key, kRows},
                              {"Reeot_BindsPad", w.pad, kRows},
                              {"Reeot_BindsHead", w.header, 3}};
    for (const Column &column : columns)
      for (uint32_t i = 0; i < column.count; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "%s%02u", column.prefix, i);
        column.slots[i] = hud::Find(ctx, base, NameCrc(name));
      }
    w.footer = hud::Find(ctx, base, NameCrc("Reeot_BindsFooter"));
  }
  w.found = w.panel != hud::kNoWindow;
  if (!w.found)
    EOT_WARN("[menu] {} is not there; is ReeotMenu.pkz mounted?", layout.panel);
  return w.found;
}

void DressWindows(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  if (w.scroll_down == hud::kNoWindow)
    return;
  const float corners[8] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const uint32_t scratch = g_block + kBlockText;
  for (uint32_t i = 0; i < 8; ++i)
    eot::mem::store<uint32_t>(scratch + i * 4, std::bit_cast<uint32_t>(corners[i]));
  for (uint32_t wide = 0; wide <= 1; ++wide)
    hud::Call(ctx, base, hud::kWnd2DSetUVs, w.scroll_down, scratch, scratch + 8, scratch + 16, scratch + 24, wide);
}

void FillConfig(uint32_t c, uint32_t window, uint32_t title, bool blackout) {
  for (uint32_t off = 0; off < cfg::kSize; off += 4)
    eot::mem::store<uint32_t>(c + off, 0);
  eot::mem::store<uint32_t>(c + cfg::kVtable, cfg::kGameOptionsVtable);
  eot::mem::store<uint32_t>(c + cfg::kPriority, 0);
  eot::mem::store<uint32_t>(c + cfg::kPadMask, kPadMask);
  eot::mem::store<float>(c + cfg::kInputDelay, 0.0f);
  eot::mem::store<float>(c + cfg::kOpenTime, 0.25f);
  eot::mem::store<float>(c + cfg::kCloseTime, 0.15f);
  eot::mem::store<float>(c + cfg::kBackdropAlpha, blackout ? 1.0f : 0.65f);
  eot::mem::store<uint8_t>(c + cfg::kFlags, static_cast<uint8_t>(cfg::kFlagOwnedByCaller | cfg::kFlagInputBlock |
                                                                 (blackout ? cfg::kFlagBlackout : 0)));
  for (uint32_t i = 0; i < cfg::kTitleCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kTitle + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kTitle, title);
  eot::mem::store<uint32_t>(c + cfg::kWindow, window);
  for (uint32_t i = 0; i < cfg::kAuxWindowCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kAuxWindow + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kOne, 1);
  eot::mem::store<uint32_t>(c + cfg::kPage, 0);
  eot::mem::store<uint32_t>(c + cfg::kBody, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kResult, cfg::kResultNone);
}

void OpenPage(const PPCContext &ctx, uint8_t *base, const Page &page) {
  CallScope scope(ctx, base);
  g_page = &page;
  RefreshLanguageChoices();
  if (page.table) {
    g_mods_message.clear();
    g_remove_armed.clear();
    g_mods_changed = false;
    g_prompts_sent = false;
    g_import_state = mods::Api().import_state ? mods::Api().import_state(nullptr, 0) : EOT_MODS_IDLE;
    RefreshMods();
    if (!mods::Api().Bound())
      EOT_WARN("[menu] the host has no mods entry points; the Mods page can do nothing");
  }
  if (page.binds) {
    g_prompts_sent = false;
    g_bind_column = kColumnKey;
    g_capture = Capture{};
    g_bind_fixed_note = false;
    if (binds::Api().capture_end)
      binds::Api().capture_end();
    if (!binds::Api().Bound())
      EOT_WARN("[menu] the host has no bind entry points; the Configure Buttons page can only show the binds");
  }
  if (!g_config)
    g_config = AllocGuest(ctx, base, cfg::kSize);
  if (!g_block)
    g_block = AllocGuest(ctx, base, kBlockSize);
  if (!g_config || !g_block || !FindWindows(ctx, base)) {
    EOT_WARN("[menu] {} page: config {:#x} block {:#x}; not opening", page.label, g_config, g_block);
    g_page = nullptr;
    return;
  }
  BuildRows(ctx, base);
  DressWindows(ctx, base);

  g_cursor = 0;
  g_first = 0;
  g_held = 0.0;
  g_held_dir = 0;
  g_opened_with.clear();
  for (const Setting &s : page.settings)
    g_opened_with.push_back(Value(s));

  FillConfig(g_config, CurrentWindows().panel, StringHandle(ctx, base, page.title), page.binds);
  PPCContext call = ctx;
  call.r3.u32 = g_config;
  __imp__eot_YesNoWindow_Open(call, base);
  g_popup_id = call.r3.u32;
  g_popup_open = g_popup_id != 0xFFFFFFFFu;
  EOT_DEBUG("[menu] {} page: {} {}, panel {:#x} -> pop-up id {:#x}", page.label,
            page.table ? g_mods.size() : page.binds ? kBindRowCount : page.settings.size(),
            page.table ? "mods" : page.binds ? "binds" : "settings", CurrentWindows().panel, g_popup_id);
}

bool RestartDue() {
  if (g_page->table)
    return g_mods_changed;
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
    if (g_page->settings[i].restart && Value(g_page->settings[i]) != g_opened_with[i])
      return true;
  return false;
}

bool RetailChanged() {
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
    if (g_page->settings[i].accessor && Value(g_page->settings[i]) != g_opened_with[i])
      return true;
  return false;
}

void UndoRestartBound() {
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i) {
    const Setting &s = g_page->settings[i];
    if (s.restart && Value(s) != g_opened_with[i]) {
      SetValue(s, g_opened_with[i]);
      EOT_DEBUG("[menu] {} back to {} (no restart)", Name(s), g_opened_with[i]);
    }
  }
  rex::cvar::InvokeCommand("eot_save_settings", "");
}

void AskRestart(const PPCContext &ctx, uint8_t *base) {
  if (!g_restart_config)
    g_restart_config = AllocGuest(ctx, base, cfg::kSize);
  if (!g_restart_config)
    return;
  for (uint32_t off = 0; off < cfg::kSize; off += 4)
    eot::mem::store<uint32_t>(g_restart_config + off, 0);
  PPCContext call = ctx;
  call.r3.u32 = g_restart_config;
  __imp__eot_YesNoWindow_InitConfig(call, base);
  eot::mem::store<uint32_t>(g_restart_config + cfg::kVtable, cfg::kYesNoVtable);
  eot::mem::store<uint32_t>(g_restart_config + cfg::kTitle, StringHandle(ctx, base, "REEOT_RESTART_TITLE"));
  eot::mem::store<uint32_t>(g_restart_config + cfg::kBody, StringHandle(ctx, base, "REEOT_RESTART_BODY"));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgWindow, hud::Find(ctx, base, NameCrc("Reeot_LeaveWindow")));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgYes, hud::Find(ctx, base, NameCrc("Reeot_LeaveYes")));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgNo, hud::Find(ctx, base, NameCrc("Reeot_LeaveNo")));
  call = ctx;
  call.r3.u32 = g_restart_config;
  __imp__eot_YesNoWindow_Open(call, base);
  g_restart_asked = call.r3.u32 != 0xFFFFFFFFu;
  EOT_DEBUG("[menu] restart asked -> pop-up id {:#x}", call.r3.u32);
}

void Close(const PPCContext &ctx, uint8_t *base, bool accept) {
  bool restart = false;
  uint32_t result = cfg::kResultCancel;
  if (!accept) {
    for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
      if (Value(g_page->settings[i]) != g_opened_with[i])
        SetValue(g_page->settings[i], g_opened_with[i]);
  } else {
    restart = RestartDue();
    result = RetailChanged() ? cfg::kResultAcceptSave : cfg::kResultAccept;
    if (!rex::cvar::InvokeCommand("eot_save_settings", ""))
      EOT_WARN("[menu] eot_save_settings is not registered; the settings hold until exit");
  }
  eot::mem::store<uint32_t>(g_config + cfg::kResult, result);
  PlayCue(ctx, base, accept && !g_page->table && !g_page->binds ? kCueAccept : kCueBack);
  if (restart)
    AskRestart(ctx, base);
}

uint32_t RowCount() {
  if (g_page->table)
    return static_cast<uint32_t>(g_mods.size());
  if (g_page->binds)
    return kBindRowCount;
  return static_cast<uint32_t>(g_page->settings.size());
}

void MoveCursor(const PPCContext &ctx, uint8_t *base, int step) {
  const int count = static_cast<int>(RowCount());
  const int next = static_cast<int>(g_cursor) + step;
  if (next < 0 || next >= count)
    return;
  g_cursor = static_cast<uint32_t>(next);
  if (g_cursor < g_first)
    g_first = g_cursor;
  else if (g_cursor >= g_first + kRows)
    g_first = g_cursor - kRows + 1;
  PlayCue(ctx, base, step > 0 ? kCueDown : kCueUp);
  ShowRows(ctx, base);
}

void ChangeValue(const PPCContext &ctx, uint8_t *base, int step) {
  const Setting &s = g_page->settings[g_cursor];
  if (s.IsButton())
    return;
  if (!Enabled(s)) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const bool moved = s.IsSlider() ? StepSlider(s, step) : StepChoice(s, step);
  if (!moved) {
    if (!s.IsSlider()) {
      EOT_WARN("[menu] {} refused the change", Name(s));
      PlayCue(ctx, base, kCueDenied);
    }
    return;
  }
  EOT_INFO("[menu] {} = {}", Name(s), Value(s));
  PlayCue(ctx, base, kCueChange);
  ShowRows(ctx, base);
}

void NoteModsResult(const PPCContext &ctx, uint8_t *base, bool ok, const char *message) {
  g_mods_message = message;
  g_mods_changed = g_mods_changed || ok;
  g_remove_armed.clear();
  PlayCue(ctx, base, ok ? kCueAccept : kCueDenied);
  EOT_DEBUG("[menu] mods: {}", g_mods_message);
  RefreshMods();
  if (g_cursor >= g_mods.size())
    g_cursor = g_mods.empty() ? 0 : static_cast<uint32_t>(g_mods.size()) - 1;
  if (g_first > g_cursor)
    g_first = g_cursor;
  ShowRows(ctx, base);
}

std::string BoundKey(const char *cvar) {
  std::string value = rex::cvar::GetFlagByName(cvar);
  if (const size_t comma = value.find(','); comma != std::string::npos)
    value.resize(comma);
  if (const size_t plus = value.rfind('+'); plus != std::string::npos)
    value.erase(0, plus + 1);
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(0, 1);
  return value;
}

eot::controller::PadInput PhysicalOf(const eot::controller::PadAction &action) {
  const eot::controller::PadInput bound = eot::controller::ParsePadInput(rex::cvar::GetFlagByName(action.pad_cvar));
  return bound == eot::controller::PadInput::None ? action.native : bound;
}

bool SticksSwapped() { return rex::cvar::GetFlagByName(eot::controller::kPadSticksCvar) == "swapped"; }

uint8_t KeySlotOf(const eot::controller::PadAction &action) {
  using eot::controller::PadInput;
  if (action.native == PadInput::LS)
    return 0x0C;
  if (action.native == PadInput::RS)
    return 0x0D;
  if (action.native == PadInput::LSRS)
    return eot::controller::kTimeParadoxCapSlot;
  return eot::controller::PadInputSlot(action.native);
}

const char *KeyGlyphString(uint8_t slot) {
  static char name[16];
  std::snprintf(name, sizeof(name), "REEOT_GK_%02X", slot);
  return name;
}
const char *PadGlyphString(uint8_t slot) {
  static char name[16];
  const bool follows = binds::Api().pad_glyph && binds::Api().pad_glyph(slot) != 0;
  std::snprintf(name, sizeof(name), follows ? "REEOT_GM_%02X" : "REEOT_GP_%02X", slot);
  return name;
}

bool KeyHasCap(uint8_t slot) { return binds::Api().key_glyph && binds::Api().key_glyph(slot) != 0; }

void ShowKey(const PPCContext &ctx, uint8_t *base, uint32_t window, const std::string &key, uint8_t slot) {
  if (key.empty())
    SetStringHandle(ctx, base, window, StringHandle(ctx, base, "REEOT_BINDS_NONE"));
  else if (KeyHasCap(slot))
    SetStringHandle(ctx, base, window, StringHandle(ctx, base, KeyGlyphString(slot)));
  else
    SetLine(ctx, base, window, key.c_str());
}

void ShowKeyCell(const PPCContext &ctx, uint8_t *base, uint32_t window, const BindRow &row) {
  if (row.kind == BindKind::kAction) {
    const eot::controller::PadAction &action = eot::controller::kPadActions[row.action];
    ShowKey(ctx, base, window, BoundKey(action.key_cvar), KeySlotOf(action));
    return;
  }
  ShowKey(ctx, base, window, BoundKey(row.cvar), row.key_slot);
}

void ShowPadCell(const PPCContext &ctx, uint8_t *base, uint32_t window, const BindRow &row) {
  using eot::controller::PadInput;
  using eot::controller::PadInputSlot;
  if (row.kind == BindKind::kStick) {
    const bool right = row.right_stick != SticksSwapped();
    const uint8_t *slots = right ? eot::controller::kRightStickDirSlots : eot::controller::kLeftStickDirSlots;
    const uint8_t *own = row.right_stick ? eot::controller::kRightStickDirSlots : eot::controller::kLeftStickDirSlots;
    uint32_t d = 0;
    for (uint32_t i = 0; i < 4; ++i)
      if (own[i] == row.pad_slot)
        d = i;
    SetStringHandle(ctx, base, window, StringHandle(ctx, base, PadGlyphString(slots[d])));
    return;
  }
  PadInput input = PhysicalOf(eot::controller::kPadActions[row.action]);
  if (input == PadInput::LS || input == PadInput::RS) {
    const bool right = (input == PadInput::RS) != SticksSwapped();
    SetStringHandle(ctx, base, window,
                    StringHandle(ctx, base, PadGlyphString(right ? eot::controller::kRightStickPressedSlot
                                                                 : eot::controller::kLeftStickPressedSlot)));
    return;
  }
  if (input == PadInput::LSRS) {
    const bool follows = binds::Api().pad_glyph &&
                         binds::Api().pad_glyph(eot::controller::kLeftStickPressedSlot) != 0;
    SetStringHandle(ctx, base, window,
                    StringHandle(ctx, base, follows ? "REEOT_GM_BOTHSTICKS" : "REEOT_GP_BOTHSTICKS"));
    return;
  }
  const uint8_t slot = PadInputSlot(input);
  if (slot != 0xFF) {
    SetStringHandle(ctx, base, window, StringHandle(ctx, base, PadGlyphString(slot)));
    return;
  }
  const char *text = input == PadInput::Down ? "REEOT_BINDS_PAD_DOWN"
                     : input == PadInput::Left ? "REEOT_BINDS_PAD_LEFT"
                     : input == PadInput::Right ? "REEOT_BINDS_PAD_RIGHT"
                                                 : "REEOT_BINDS_NONE";
  SetStringHandle(ctx, base, window, StringHandle(ctx, base, text));
}

void ShowBindNote(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  if (w.info_note == hud::kNoWindow)
    return;
  hud::Activate(ctx, base, w.info_note, true);
  if (g_capture.active) {
    SetStringHandle(ctx, base, w.info_note,
                    StringHandle(ctx, base, g_capture.column == kColumnPad ? "REEOT_BINDS_PRESS_PAD"
                                                                           : "REEOT_BINDS_PRESS_KEY"));
    return;
  }
  SetStringHandle(ctx, base, w.info_note, StringHandle(ctx, base, g_bind_fixed_note ? "REEOT_BINDS_FIXED" : "REEOT_BINDS_HINT"));
}

void ShowBindRows(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = g_first + row;
    const bool used = index < kBindRowCount;
    for (const uint32_t *column : {w.row, w.action, w.key, w.pad})
      hud::Activate(ctx, base, column[row], used);
    if (!used)
      continue;
    const BindRow &bind = kBindRows[index];
    SetStringHandle(ctx, base, w.action[row], StringHandle(ctx, base, bind.label));
    ShowKeyCell(ctx, base, w.key[row], bind);
    ShowPadCell(ctx, base, w.pad[row], bind);
    const bool selected = index == g_cursor;
    ShadeWindow(ctx, base, w.action[row], selected ? kShadeSelected : kShadeIdle);
    ShadeWindow(ctx, base, w.key[row], selected && g_bind_column == kColumnKey ? kShadeSelected : kShadeIdle);
    ShadeWindow(ctx, base, w.pad[row], selected && g_bind_column == kColumnPad ? kShadeSelected : kShadeIdle);
  }
  hud::Activate(ctx, base, w.scroll_up, g_first > 0);
  hud::Activate(ctx, base, w.scroll_down, g_first + kRows < kBindRowCount);
  SetStringHandle(ctx, base, w.footer, StringHandle(ctx, base, "REEOT_BINDS_FOOTER"));
  const BindRow &bind = kBindRows[std::min(g_cursor, kBindRowCount - 1)];
  const uint32_t label = StringHandle(ctx, base, bind.label);
  FitInfoTitle(ctx, base, StringLength(ctx, base, label));
  SetStringHandle(ctx, base, w.info_title, label);
  SetStringHandle(ctx, base, w.info_text, StringHandle(ctx, base, bind.description));
  ShowBindNote(ctx, base);
}

void BindsChanged() {
  if (binds::Api().changed)
    binds::Api().changed();
  g_bind_redraw_in = kBindRedrawFrames;
}

void StartCapture(const PPCContext &ctx, uint8_t *base) {
  const BindRow &row = kBindRows[g_cursor];
  const binds::Host &api = binds::Api();
  g_bind_fixed_note = false;
  if (row.kind == BindKind::kStick && g_bind_column == kColumnPad) {
    rex::cvar::SetFlagByName(eot::controller::kPadSticksCvar, SticksSwapped() ? "normal" : "swapped");
    BindsChanged();
    EOT_INFO("[menu] sticks {}", SticksSwapped() ? "swapped" : "normal");
    PlayCue(ctx, base, kCueChange);
    ShowRows(ctx, base);
    return;
  }
  if (!api.Bound()) {
    g_bind_fixed_note = true;
    PlayCue(ctx, base, kCueDenied);
    ShowBindNote(ctx, base);
    return;
  }
  const int32_t kind = g_bind_column == kColumnPad ? EOT_BINDS_PAD : EOT_BINDS_KEY;
  if (!api.capture_begin(kind)) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  g_capture.active = true;
  g_capture.row = g_cursor;
  g_capture.column = g_bind_column;
  PlayCue(ctx, base, kCueChange);
  ShowBindNote(ctx, base);
}

void EndCapture(const PPCContext &ctx, uint8_t *base, bool changed) {
  if (binds::Api().capture_end)
    binds::Api().capture_end();
  g_capture = Capture{};
  if (changed)
    BindsChanged();
  PlayCue(ctx, base, changed ? kCueAccept : kCueBack);
  ShowRows(ctx, base);
}

void PollCapture(const PPCContext &ctx, uint8_t *base) {
  const binds::Host &api = binds::Api();
  char name[64] = {};
  const int32_t state = api.capture_poll ? api.capture_poll(name, sizeof(name)) : EOT_BINDS_CANCELLED;
  if (state == EOT_BINDS_WAITING)
    return;
  const BindRow &row = kBindRows[g_capture.row];
  const char *key_cvar = row.kind == BindKind::kAction ? eot::controller::kPadActions[row.action].key_cvar : row.cvar;
  switch (state) {
  case EOT_BINDS_GOT_KEY:
    rex::cvar::SetFlagByName(key_cvar, name);
    EOT_INFO("[menu] {} = {}", key_cvar, name);
    EndCapture(ctx, base, true);
    return;
  case EOT_BINDS_GOT_PAD:
    if (row.kind == BindKind::kAction) {
      rex::cvar::SetFlagByName(eot::controller::kPadActions[row.action].pad_cvar, name);
      EOT_INFO("[menu] {} = {}", eot::controller::kPadActions[row.action].pad_cvar, name);
    }
    EndCapture(ctx, base, true);
    return;
  case EOT_BINDS_CLEARED:
    if (g_capture.column == kColumnKey) {
      rex::cvar::SetFlagByName(key_cvar, "");
      EOT_INFO("[menu] {} cleared", key_cvar);
      EndCapture(ctx, base, true);
      return;
    }
    EndCapture(ctx, base, false);
    return;
  default:
    EndCapture(ctx, base, false);
    return;
  }
}

void ResetBind(const PPCContext &ctx, uint8_t *base, bool all) {
  const uint32_t from = all ? 0 : g_cursor;
  const uint32_t to = all ? kBindRowCount : g_cursor + 1;
  for (uint32_t i = from; i < to; ++i) {
    const BindRow &row = kBindRows[i];
    const bool key = all || g_bind_column == kColumnKey;
    const bool pad = all || g_bind_column == kColumnPad;
    if (row.kind == BindKind::kAction) {
      if (key)
        rex::cvar::ResetToDefault(eot::controller::kPadActions[row.action].key_cvar);
      if (pad)
        rex::cvar::ResetToDefault(eot::controller::kPadActions[row.action].pad_cvar);
    } else {
      if (key)
        rex::cvar::ResetToDefault(row.cvar);
      if (pad)
        rex::cvar::ResetToDefault(eot::controller::kPadSticksCvar);
    }
  }
  EOT_INFO("[menu] binds: {} back to default", all ? "everything" : kBindRows[g_cursor].label);
  BindsChanged();
  PlayCue(ctx, base, kCueChange);
  ShowRows(ctx, base);
}

void ShowBindsPrompts(const PPCContext &ctx, uint8_t *base) {
  if (g_prompts_sent)
    return;
  g_prompts_sent = true;
  OverridePrompt(kPromptA, "REEOT_PROMPT_REBIND");
  OverridePrompt(kPromptX, "REEOT_PROMPT_RESET");
  OverridePrompt(kPromptY, "REEOT_PROMPT_DEFAULTS");
  SendPromptMask(ctx, base, 0, (1u << kPromptA) | (1u << kPromptBack) | (1u << kPromptX) | (1u << kPromptY),
                 g_block + kBlockColour);
}

void ToggleMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (g_cursor >= g_mods.size() || !ImportIdle() || !api.Bound()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const eot_mod_info &mod = g_mods[g_cursor];
  char message[512];
  const bool ok = api.set_enabled(mod.folder, mod.enabled ? 0 : 1, message, sizeof(message)) != 0;
  NoteModsResult(ctx, base, ok, message);
}

void RemoveMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (g_cursor >= g_mods.size() || !ImportIdle() || !api.Bound()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const eot_mod_info &mod = g_mods[g_cursor];
  if (g_remove_armed != mod.folder) {
    g_remove_armed = mod.folder;
    g_mods_message.clear();
    PlayCue(ctx, base, kCueChange);
    ShowRows(ctx, base);
    return;
  }
  char message[512];
  const bool ok = api.remove(mod.folder, message, sizeof(message)) != 0;
  NoteModsResult(ctx, base, ok, message);
}

void AddMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (!ImportIdle() || !api.Bound() || !api.add_begin()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  g_mods_message.clear();
  g_remove_armed.clear();
  g_import_state = EOT_MODS_CHOOSING;
  PlayCue(ctx, base, kCueAccept);
  EOT_DEBUG("[menu] mods: the file browser is up");
  ShowRows(ctx, base);
}

void PollImport(const PPCContext &ctx, uint8_t *base) {
  if (!mods::Api().import_state)
    return;
  char message[512];
  const int32_t state = mods::Api().import_state(message, sizeof(message));
  if (state == g_import_state)
    return;
  g_import_state = state;
  if (state == EOT_MODS_DONE || state == EOT_MODS_FAILED) {
    mods::Api().import_acknowledge();
    g_import_state = EOT_MODS_IDLE;
    NoteModsResult(ctx, base, state == EOT_MODS_DONE, message);
    return;
  }
  ShowRows(ctx, base);
}

void ShowTablePrompts(const PPCContext &ctx, uint8_t *base) {
  if (g_prompts_sent)
    return;
  g_prompts_sent = true;
  OverridePrompt(kPromptA, "REEOT_PROMPT_TOGGLE");
  OverridePrompt(kPromptX, "REEOT_PROMPT_REMOVE");
  OverridePrompt(kPromptY, "REEOT_PROMPT_ADD");
  SendPromptMask(ctx, base, 0, (1u << kPromptA) | (1u << kPromptBack) | (1u << kPromptX) | (1u << kPromptY),
                 g_block + kBlockColour);
}

void RestoreTablePrompts(const PPCContext &ctx, uint8_t *base) {
  if (!g_prompts_sent)
    return;
  g_prompts_sent = false;
  SendPromptMask(ctx, base, 0, 0, g_block + kBlockColour);
  RestorePrompt(kPromptA);
  RestorePrompt(kPromptX);
  RestorePrompt(kPromptY);
}

void PollHorizontal(const PPCContext &ctx, uint8_t *base, double dt) {
  const double axis = Axis(ctx, base, kInputAxisX);
  const int dir = axis > kAxisHeld ? 1 : axis < -kAxisHeld ? -1 : 0;
  if (Pressed(ctx, base, kInputAxisX)) {
    ChangeValue(ctx, base, axis > 0.0 ? 1 : -1);
    g_held = 0.0;
    g_held_dir = dir;
    return;
  }
  if (!dir || dir != g_held_dir || !g_page->settings[g_cursor].IsSlider()) {
    g_held = 0.0;
    g_held_dir = dir;
    return;
  }
  g_held += dt;
  if (g_held >= kRepeatDelay) {
    g_held -= kRepeatInterval;
    ChangeValue(ctx, base, dir);
  }
}

}

REX_HOOK_RAW(eot_HUDOptionsScreen_BuildBar) {
  g_video_label = StringHandle(ctx, base, "REEOT_VIDEO");
  g_graphics_label = StringHandle(ctx, base, "REEOT_GRAPHICS");
  g_mods_label = ModsOnBar() ? StringHandle(ctx, base, "REEOT_MODS") : 0;
  __imp__eot_HUDOptionsScreen_BuildBar(ctx, base);
}

void eot_OptionsBar_AddGraphics(PPCRegister &r6, PPCRegister &r31) {
  const uint32_t desc = r6.u32;
  if (!desc || eot::mem::load<uint32_t>(desc + kDescCount) != kRetailCount || !g_video_label || !g_graphics_label)
    return;
  eot::mem::store<uint32_t>(DescHandleAddr(desc, kVideoIndex), g_video_label);
  for (uint32_t i = kRetailCount; i > kGraphicsIndex; --i) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, i), eot::mem::load<uint32_t>(DescHandleAddr(desc, i - 1)));
    eot::mem::store<uint32_t>(DescPropAddr(desc, i), eot::mem::load<uint32_t>(DescPropAddr(desc, i - 1)));
  }
  eot::mem::store<uint32_t>(DescHandleAddr(desc, kGraphicsIndex), g_graphics_label);
  eot::mem::store<uint32_t>(DescPropAddr(desc, kGraphicsIndex), kPropDefault);
  eot::mem::store<uint32_t>(desc + kDescCount, kRetailCount + 1);
  const uint16_t mask = eot::mem::load<uint16_t>(desc + kDescSelectableMask);
  const uint16_t above = static_cast<uint16_t>((mask & ~((1u << kGraphicsIndex) - 1)) << 1);
  const uint16_t below = static_cast<uint16_t>(mask & ((1u << kGraphicsIndex) - 1));
  eot::mem::store<uint16_t>(desc + kDescSelectableMask, static_cast<uint16_t>(above | below | (1u << kGraphicsIndex)));
  eot::mem::store<uint32_t>(desc + kDescSelected, 0);
  if (r31.u32)
    eot::mem::store<uint32_t>(r31.u32 + kScreenCursorOff, 0);
  const int mods_slot = g_mods_label ? AppendEntry(desc, g_mods_label) : -1;
  EOT_DEBUG("[menu] options bar {:#x}: Video in slot {}, Graphics in slot {}{}", desc, kVideoIndex, kGraphicsIndex,
            mods_slot >= 0 ? std::format(", Mods in slot {}", mods_slot) : std::string());
}

void eot_OptionsBar_NavRightBound6(PPCRegister &r29, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<uint32_t>(r29.u32, LastBarIndex(), xer);
}

// Sent as a message so the Options screen, not the closing Video page, owns the
// pop-up and hears the result that makes it active again.
void OpenRetailBrightness(const PPCContext &ctx, uint8_t *base, uint32_t screen_handle) {
  const uint32_t screen = g_options_screen;
  if (!screen || !screen_handle || screen_handle == 0xFFFFFFFFu) {
    EOT_WARN("[menu] no Options screen ({:#x}, handle {:#x}); the Brightness screen does not open", screen,
             screen_handle);
    return;
  }
  const uint32_t api = eot::mem::load<uint32_t>(kApiLogicPtr);
  if (!g_event)
    g_event = AllocGuest(ctx, base, kEventSize);
  if (!g_event || !api)
    return;
  for (uint32_t off = 0; off < kEventSize; off += 4)
    eot::mem::store<uint32_t>(g_event + off, 0);
  eot::mem::store<uint32_t>(g_event + kEvtType, kEvtSelect);
  eot::mem::store<uint8_t>(g_event + kEvtConsumed, 1);
  g_brightness_select = true;
  hud::CallAt(ctx, base, eot::mem::load<uint32_t>(api), screen_handle, 0, kMsgInputEvent, g_event);
  if (g_brightness_select) {
    g_brightness_select = false;
    EOT_WARN("[menu] the Options screen did not take the Brightness select; the screen does not open");
    return;
  }
  EOT_DEBUG("[menu] the game's Brightness screen opened from the Video page");
}

REX_HOOK_RAW(eot_HUDOptionsScreen_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (self)
    g_options_screen = self;
  if (g_popup_open || g_restart_asked) {
    ConsumeEvent(event);
    return;
  }
  if (g_brightness_select && type == kEvtSelect && self) {
    g_brightness_select = false;
    const uint32_t cursor = eot::mem::load<uint32_t>(self + kScreenCursorOff);
    eot::mem::store<uint32_t>(self + kScreenCursorOff, kRetailBrightnessIndex);
    __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
    eot::mem::store<uint32_t>(self + kScreenCursorOff, cursor);
    return;
  }
  if (type == kEvtSelect && self) {
    const uint32_t cursor = eot::mem::load<uint32_t>(self + kScreenCursorOff);
    if (const Page *page = PageAt(cursor)) {
      OpenPage(ctx, base, *page);
      ConsumeEvent(event);
      return;
    }
    if (cursor == kDifficultyIndex) {
      eot::mem::store<uint32_t>(self + kScreenCursorOff, cursor - 1);
      __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
      eot::mem::store<uint32_t>(self + kScreenCursorOff, cursor);
      return;
    }
  }
  __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_TCRWindow_OpeningEnter) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t slot_ptr = eot::mem::load<uint32_t>(tcr::kCurrentSlotPtr);
  const uint32_t slot = slot_ptr >= tcr::kSlotTable ? (slot_ptr - tcr::kSlotTable) / tcr::kSlotSize : 0xFFFFu;
  const bool ours = g_popup_open && g_page && slot == (g_popup_id & 0xFFFFu);
  if (self) {
    if (!g_retail_tween_saved) {
      for (uint32_t i = 0; i < 8; ++i)
        g_retail_tween[i] = std::bit_cast<float>(eot::mem::load<uint32_t>(self + tcr::kTweenStart + i * 4));
      g_retail_tween_saved = true;
    }
    float rects[8];
    if (ours) {
      const float *box = CurrentLayout().box;
      const float sw = box[2] * tcr::kStartScale, sh = box[3] * tcr::kStartScale;
      const float start[4] = {box[0] + (box[2] - sw) * 0.5f, box[1] + (box[3] - sh) * 0.5f, sw, sh};
      std::copy(start, start + 4, rects);
      std::copy(box, box + 4, rects + 4);
    } else {
      std::copy(g_retail_tween, g_retail_tween + 8, rects);
    }
    for (uint32_t i = 0; i < 8; ++i)
      eot::mem::store<uint32_t>(self + tcr::kTweenStart + i * 4, std::bit_cast<uint32_t>(rects[i]));
  }
  __imp__eot_TCRWindow_OpeningEnter(ctx, base);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnShow) {
  if (ctx.r3.u32 != g_config || !g_config) {
    __imp__eot_GameOptionsPopup_OnShow(ctx, base);
    return;
  }
  CallScope scope(ctx, base);
  const bool shown = (ctx.r4.u32 & 0xFF) != 0;
  ScaleText(ctx, base, g_title, shown ? kTextScale : 1.0f);
  if (shown && g_page)
    ShowRows(ctx, base);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnUpdate) {
  if (ctx.r3.u32 != g_config || !g_config || !g_page) {
    __imp__eot_GameOptionsPopup_OnUpdate(ctx, base);
    return;
  }
  CallScope scope(ctx, base);
  const double dt = ctx.f1.f64;
  ctx.r3.u32 = 0;
  if (g_page->table) {
    ShowTablePrompts(ctx, base);
    PollImport(ctx, base);
    if (Pressed(ctx, base, kInputAccept)) {
      ToggleMod(ctx, base);
    } else if (Pressed(ctx, base, kInputBack)) {
      Close(ctx, base, true);
      ctx.r3.u32 = 1;
    } else if (Pressed(ctx, base, kInputRemove)) {
      RemoveMod(ctx, base);
    } else if (Pressed(ctx, base, kInputReset)) {
      AddMod(ctx, base);
    } else if (Pressed(ctx, base, kInputAxisY)) {
      g_remove_armed.clear();
      MoveCursor(ctx, base, Axis(ctx, base, kInputAxisY) > 0.0 ? 1 : -1);
    }
    return;
  }
  if (g_page->binds) {
    ShowBindsPrompts(ctx, base);
    if (g_bind_redraw_in && --g_bind_redraw_in == 0)
      ShowRows(ctx, base);
    if (g_capture.active) {
      PollCapture(ctx, base);
      if (g_capture.active && Pressed(ctx, base, kInputBack))
        EndCapture(ctx, base, false);
      return;
    }
    const bool ctrl = binds::Api().ctrl_pressed && binds::Api().ctrl_pressed() != 0;
    if (ctrl || Pressed(ctx, base, kInputAccept)) {
      StartCapture(ctx, base);
    } else if (Pressed(ctx, base, kInputBack)) {
      Close(ctx, base, true);
      ctx.r3.u32 = 1;
    } else if (Pressed(ctx, base, kInputRemove)) {
      ResetBind(ctx, base, false);
    } else if (Pressed(ctx, base, kInputReset)) {
      ResetBind(ctx, base, true);
    } else if (Pressed(ctx, base, kInputAxisY)) {
      g_bind_fixed_note = false;
      MoveCursor(ctx, base, Axis(ctx, base, kInputAxisY) > 0.0 ? 1 : -1);
    } else if (Pressed(ctx, base, kInputAxisX)) {
      g_bind_fixed_note = false;
      g_bind_column = g_bind_column == kColumnKey ? kColumnPad : kColumnKey;
      PlayCue(ctx, base, kCueChange);
      ShowRows(ctx, base);
    }
    return;
  }
  if (Pressed(ctx, base, kInputAccept)) {
    if (g_cursor < g_page->settings.size())
      g_pending_opens = g_page->settings[g_cursor].opens;
    Close(ctx, base, true);
    ctx.r3.u32 = 1;
  } else if (Pressed(ctx, base, kInputBack)) {
    Close(ctx, base, false);
    ctx.r3.u32 = 1;
  } else if (Pressed(ctx, base, kInputReset)) {
    for (const Setting &s : g_page->settings)
      ResetValue(s);
    PlayCue(ctx, base, kCueChange);
    ShowRows(ctx, base);
  } else if (Pressed(ctx, base, kInputAxisY)) {
    MoveCursor(ctx, base, Axis(ctx, base, kInputAxisY) > 0.0 ? 1 : -1);
  } else {
    PollHorizontal(ctx, base, dt);
  }
}

REX_HOOK_RAW(eot_WindowComponent_Teardown) {
  const uint32_t config = ctx.r3.u32 ? eot::mem::load<uint32_t>(ctx.r3.u32 + kComponentConfigOff) : 0;
  const uint32_t owner = ctx.r3.u32 ? eot::mem::load<uint32_t>(ctx.r3.u32 + kComponentOwnerOff) : 0;
  const uint32_t answer = config ? eot::mem::load<uint32_t>(config + cfg::kYesNoChoice) : 0;
  __imp__eot_WindowComponent_Teardown(ctx, base);
  CallScope scope(ctx, base);
  if (config && config == g_config) {
    g_popup_open = false;
    RestoreTablePrompts(ctx, base);
    if (g_capture.active && binds::Api().capture_end)
      binds::Api().capture_end();
    g_capture = Capture{};
    EOT_DEBUG("[menu] {} page closed (id {:#x})", g_page ? g_page->label : "?", g_popup_id);
    const Opens next = g_pending_opens;
    g_pending_opens = Opens::kNothing;
    if (next != Opens::kNothing && g_restart_asked)
      EOT_DEBUG("[menu] a restart is being asked; the button's page does not open");
    else if (next == Opens::kBinds)
      OpenPage(ctx, base, g_binds_page);
    else if (next == Opens::kBrightness)
      OpenRetailBrightness(ctx, base, owner);
  } else if (config && config == g_restart_config && g_restart_asked) {
    g_restart_asked = false;
    EOT_INFO("[menu] restart {}", answer == cfg::kYesNoYes ? "accepted" : "declined");
    if (answer == cfg::kYesNoYes)
      eot::RestartProcessFromModule();
    else if (g_page)
      UndoRestartBound();
  }
}

REX_EXTERN(__imp__eot_PauseMenu_GetMainMenuBarInfo);
REX_EXTERN(__imp__eot_PauseMenu_HandleMainMenuSelectOption);
REX_EXTERN(__imp__eot_PauseMenu_HandleMessage);
REX_EXTERN(__imp__eot_PauseMenu_EnterOpening);

namespace {

using namespace eot::ui;

constexpr uint32_t kSelectedIndexOff = 40;
constexpr uint32_t kPauseRetailCount = 7;
constexpr uint32_t kQuitGameIndex = 6;
constexpr uint32_t kExitIndex = 7;
constexpr YesNoLayout kPauseYesNo{76, 152, 160, 184, 80};

bool g_confirm_pending = false;

}

void eot_PauseMenu_NavRightBoundFexit(PPCRegister &r31, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<int32_t>(r31.s32, static_cast<int32_t>(kPauseRetailCount + 1), xer);
}

REX_HOOK_RAW(eot_PauseMenu_GetMainMenuBarInfo) {
  const uint32_t desc = ctx.r4.u32;
  __imp__eot_PauseMenu_GetMainMenuBarInfo(ctx, base);
  const uint32_t count = desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0;
  int slot = -1;
  if (count == kPauseRetailCount) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, kQuitGameIndex), kHandleExitToMenu);
    slot = AppendEntry(desc, kHandleExitGame);
  }
  EOT_DEBUG("[menu] pause bar {:#x}: count {} -> {}, Exit Game slot {}", desc, count,
            desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0u, slot);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMainMenuSelectOption) {
  const uint32_t self = ctx.r3.u32;
  if (self && eot::mem::load<uint32_t>(self + kSelectedIndexOff) == kExitIndex) {
    if (!g_confirm_pending) {
      OpenExitConfirm(ctx, base, self, kPauseYesNo);
      g_confirm_pending = true;
    }
    return;
  }
  __imp__eot_PauseMenu_HandleMainMenuSelectOption(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kPauseYesNo))
    g_confirm_pending = false;
  __imp__eot_PauseMenu_HandleMessage(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_EnterOpening) {
  g_confirm_pending = false;
  __imp__eot_PauseMenu_EnterOpening(ctx, base);
}

REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleMessage);    // (this r3, message r4, payload r5) -> handled

namespace {

using namespace eot::ui;

constexpr YesNoLayout kSaveYesNo{68, 144, 152, 172, 72};

}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
  if (self && event && eot::mem::load<uint32_t>(event + kEvtType) == kEvtBack && !g_confirm_pending) {
    ConsumeEvent(event);
    const uint32_t handle = OpenConfirm(ctx, base, self, kSaveYesNo, kHandleLeaveTitle, kHandleLeaveBody,
                                        NameCrc("Reeot_LeaveWindow"), NameCrc("Reeot_LeaveYes"), NameCrc("Reeot_LeaveNo"));
    g_confirm_pending = handle != 0xFFFFFFFFu;
    EOT_DEBUG("[menu] save selection: B, leave-the-game window {:#x}", handle);
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kSaveYesNo)) {
    g_confirm_pending = false;
    EOT_DEBUG("[menu] save selection: leave-the-game window answered");
    ctx.r3.u32 = 1;
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleMessage(ctx, base);
}

REX_EXTERN(__imp__eot_HUDSavegameSelect_Construct);  // (this r3, ...)
REX_EXTERN(__imp__eot_GLInstanciateHUDButtonHelper);
REX_EXTERN(__imp__sub_8827CBD8);

namespace {

constexpr uint32_t kPromptTable = 0x883C9BC8; // uint32 nameCRC[32], filled by 0x8827C3E8
constexpr uint32_t kPromptCount = 32;

struct Rewrite {
  uint32_t id;
  uint32_t retail;
  const char *replacement;
};

constexpr Rewrite kRewrites[] = {
    {22, 0x3A79CC4E, "REEOT_PROMPT_EXIT_GAME"},
};

void ApplyRewrites() {
  for (const Rewrite &r : kRewrites) {
    if (r.id >= kPromptCount)
      continue;
    const uint32_t slot = kPromptTable + r.id * 4;
    if (eot::mem::load<uint32_t>(slot) != r.retail)
      continue;
    const uint32_t crc = eot::ui::NameCrc(r.replacement);
    eot::mem::store<uint32_t>(slot, crc);
    EOT_DEBUG("[menu] button prompt {} -> {} (crc {:#010x})", r.id, r.replacement, crc);
  }
}

uint32_t g_prompt_held[kPromptCount] = {};

}

REX_HOOK_RAW(eot_HUDSavegameSelect_Construct) {
  __imp__eot_HUDSavegameSelect_Construct(ctx, base);
  ApplyRewrites();
}

REX_HOOK_RAW(sub_8827CBD8) {
  constexpr uint32_t kSetMaskMessage = 0xDCDC2E9F;
  const uint32_t message = ctx.r4.u32;
  const uint32_t args = ctx.r5.u32;
  __imp__sub_8827CBD8(ctx, base);
  if (message != kSetMaskMessage || !args)
    return;
  if (const auto note = eot::ui::binds::Api().bar_zone)
    note(static_cast<int32_t>(eot::mem::load<uint32_t>(args)));
}

REX_HOOK_RAW(eot_GLInstanciateHUDButtonHelper) {
  __imp__eot_GLInstanciateHUDButtonHelper(ctx, base);
  if (const auto note = eot::ui::binds::Api().bar_object)
    note(static_cast<int32_t>(ctx.r3.u32));
}

namespace eot::ui {

void OverridePrompt(uint32_t id, const char *name) {
  if (id >= kPromptCount)
    return;
  const uint32_t slot = kPromptTable + id * 4;
  if (!g_prompt_held[id])
    g_prompt_held[id] = eot::mem::load<uint32_t>(slot);
  eot::mem::store<uint32_t>(slot, eot::ui::NameCrc(name));
}

void RestorePrompt(uint32_t id) {
  if (id >= kPromptCount || !g_prompt_held[id])
    return;
  eot::mem::store<uint32_t>(kPromptTable + id * 4, g_prompt_held[id]);
  g_prompt_held[id] = 0;
}

void SendPromptMask(const PPCContext &ctx, uint8_t *base, uint32_t zone, uint32_t mask, uint32_t scratch) {
  constexpr uint32_t kHudsDataPtr = 0x883CA288;
  constexpr uint32_t kHelperOffset = 0xA4;
  constexpr uint32_t kSetMaskMessage = 0xDCDC2E9F;
  const uint32_t api = eot::mem::load<uint32_t>(kApiLogicPtr);
  const uint32_t huds = eot::mem::load<uint32_t>(kHudsDataPtr);
  if (!api || !huds || !scratch)
    return;
  const uint32_t helper = eot::mem::load<uint32_t>(huds + kHelperOffset);
  eot::mem::store<uint32_t>(scratch, zone);
  eot::mem::store<uint32_t>(scratch + 4, mask);
  hud::CallAt(ctx, base, eot::mem::load<uint32_t>(api), helper, 0, kSetMaskMessage, scratch);
}

}
