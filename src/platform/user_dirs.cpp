// platform/user_dirs.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause


#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include "platform/user_dirs.h"

#if !defined(_WIN32)

#include <cstdlib>
#include <fstream>
#include <string>

namespace eot::platform {
namespace {

namespace fs = std::filesystem;

fs::path EnvOr(const char *name, const fs::path &fallback) {
  const char *value = std::getenv(name);
  return (value && *value) ? fs::path(value) : fallback;
}

fs::path BaseDir(const char *variable, const char *relative) {
  if (const char *value = std::getenv(variable); value && *value)
    return fs::path(value);
  const fs::path home = HomeDir();
  return home.empty() ? fs::path() : home / relative;
}

}

fs::path HomeDir() { return EnvOr("HOME", fs::path()); }
fs::path ConfigHome() { return BaseDir("XDG_CONFIG_HOME", ".config"); }
fs::path DataHome() { return BaseDir("XDG_DATA_HOME", ".local/share"); }
fs::path StateHome() { return BaseDir("XDG_STATE_HOME", ".local/state"); }
fs::path RuntimeDir() { return EnvOr("XDG_RUNTIME_DIR", "/tmp"); }

fs::path UserDir(const char *key, const char *fallback) {
  const fs::path home = HomeDir();
  if (home.empty())
    return {};
  const fs::path config = ConfigHome();
  std::ifstream in(config / "user-dirs.dirs");
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) != 0)
      continue;
    std::string value = line.substr(prefix.size());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (value.rfind("$HOME", 0) == 0)
      value = home.string() + value.substr(5);
    if (!value.empty())
      return fs::path(value);
  }
  return home / fallback;
}

}

#endif

namespace eot::platform {

namespace {

#if !defined(_WIN32)
std::string PreferredLanguage() {
  int count = 0;
  SDL_Locale **locales = SDL_GetPreferredLocales(&count);
  std::string tag;
  if (locales && count > 0 && locales[0]->language) {
    for (const char *p = locales[0]->language; *p; ++p)
      tag.push_back(static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
  }
  SDL_free(locales);
  return tag;
}
#endif

}

uint32_t SystemXLanguage() {
#if defined(_WIN32)
  switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
  case LANG_FRENCH:
    return kXLanguageFrench;
  case LANG_ITALIAN:
    return kXLanguageItalian;
  case LANG_GERMAN:
    return kXLanguageGerman;
  case LANG_SPANISH:
    return kXLanguageSpanish;
  default:
    return kXLanguageEnglish;
  }
#else
  const std::string tag = PreferredLanguage();
  if (tag == "fr")
    return kXLanguageFrench;
  if (tag == "it")
    return kXLanguageItalian;
  if (tag == "de")
    return kXLanguageGerman;
  if (tag == "es")
    return kXLanguageSpanish;
  return kXLanguageEnglish;
#endif
}

uint32_t XLanguageFor(std::string_view eot_language) {
  if (eot_language == "fr")
    return kXLanguageFrench;
  if (eot_language == "it")
    return kXLanguageItalian;
  if (eot_language == "de")
    return kXLanguageGerman;
  if (eot_language == "es")
    return kXLanguageSpanish;
  if (eot_language == "en")
    return kXLanguageEnglish;
  return SystemXLanguage();
}

std::string SystemLanguageTag() {
#if defined(_WIN32)
  wchar_t name[16] = {};
  if (::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO639LANGNAME, name, 16) > 1) {
    std::string tag;
    for (const wchar_t *p = name; *p; ++p)
      tag.push_back(static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
    return tag;
  }
#else
  if (const std::string tag = PreferredLanguage(); !tag.empty())
    return tag;
#endif
  return "en";
}

std::string TranslationTag(std::string_view eot_language) {
  return eot_language == "auto" ? SystemLanguageTag() : std::string(eot_language);
}

const char *XLanguageName(uint32_t xlanguage) {
  switch (xlanguage) {
  case kXLanguageFrench:
    return "French";
  case kXLanguageItalian:
    return "Italian";
  case kXLanguageGerman:
    return "German";
  case kXLanguageSpanish:
    return "Spanish";
  default:
    return "English";
  }
}

}
