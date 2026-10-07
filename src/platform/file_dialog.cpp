// platform/file_dialog.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/file_dialog.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

#include "core/encoding.h"
#include "core/logging.h"

namespace eot::platform {
namespace {

struct DialogResult {
  std::atomic<bool> done{false};
  std::optional<std::filesystem::path> path;
  std::string error_message;
};

void SDLCALL DialogCallback(void *userdata, const char *const *filelist, int) {
  auto *result = static_cast<DialogResult *>(userdata);
  if (!filelist) {
    result->error_message = SDL_GetError();
  } else if (filelist[0]) {
    result->path = std::filesystem::path(filelist[0]);
  }
  result->done.store(true, std::memory_order_release);
}

std::string ToSdlPattern(const wchar_t *pattern) {
  const std::string src = WideToUtf8(pattern);
  std::string out;
  size_t start = 0;
  while (start <= src.size()) {
    const size_t sep = src.find(';', start);
    std::string part = src.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
    if (part == "*.*" || part == "*")
      return "*";
    if (part.rfind("*.", 0) == 0)
      part = part.substr(2);
    if (!part.empty()) {
      if (!out.empty())
        out += ';';
      out += part;
    }
    if (sep == std::string::npos)
      break;
    start = sep + 1;
  }
  return out.empty() ? "*" : out;
}

std::optional<std::filesystem::path> RunDialog(const wchar_t *title, bool pick_folder,
                                               std::span<const FileFilter> filters) {
  DialogResult result;

  const std::string title_utf8 = WideToUtf8(title);
  std::vector<std::string> names, patterns;
  names.reserve(filters.size());
  patterns.reserve(filters.size());
  for (const auto &f : filters) {
    names.push_back(WideToUtf8(f.name));
    patterns.push_back(ToSdlPattern(f.pattern));
  }
  std::vector<SDL_DialogFileFilter> sdl_filters;
  sdl_filters.reserve(filters.size());
  for (size_t i = 0; i < filters.size(); ++i)
    sdl_filters.push_back({names[i].c_str(), patterns[i].c_str()});

  SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    EOT_WARN("SDL_CreateProperties failed: {}", SDL_GetError());
    return std::nullopt;
  }
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title_utf8.c_str());
  if (!sdl_filters.empty()) {
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, sdl_filters.data());
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                          static_cast<Sint64>(sdl_filters.size()));
  }
  SDL_ShowFileDialogWithProperties(pick_folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_OPENFILE,
                                   &DialogCallback, &result, props);
  SDL_DestroyProperties(props);

  while (!result.done.load(std::memory_order_acquire)) {
    SDL_PumpEvents();
    SDL_Delay(10);
  }
  if (!result.error_message.empty())
    EOT_WARN("SDL file dialog failed: {}", result.error_message);
  return result.path;
}

}

std::optional<std::filesystem::path> ShowOpenFileDialog(const wchar_t *title,
                                                        std::span<const FileFilter> filters) {
  return RunDialog(title, false, filters);
}

std::optional<std::filesystem::path> ShowOpenFolderDialog(const wchar_t *title) {
  return RunDialog(title, true, {});
}

}
