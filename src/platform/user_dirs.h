// platform/user_dirs.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

#if !defined(_WIN32)

namespace eot::platform {

std::filesystem::path HomeDir();

std::filesystem::path ConfigHome();
std::filesystem::path DataHome();
std::filesystem::path StateHome();

std::filesystem::path RuntimeDir();

std::filesystem::path UserDir(const char *key, const char *fallback);

}

#endif

namespace eot::platform {

constexpr uint32_t kXLanguageEnglish = 1;
constexpr uint32_t kXLanguageGerman = 3;
constexpr uint32_t kXLanguageFrench = 4;
constexpr uint32_t kXLanguageSpanish = 5;
constexpr uint32_t kXLanguageItalian = 6;

uint32_t XLanguageFor(std::string_view eot_language);

uint32_t SystemXLanguage();

const char *XLanguageName(uint32_t xlanguage);

std::string SystemLanguageTag();

std::string TranslationTag(std::string_view eot_language);

}
