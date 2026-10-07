// core/encoding.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace eot {

inline std::wstring Utf8ToWide(std::string_view text) {
#if defined(_WIN32)
  if (text.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
#else
  return std::wstring(text.begin(), text.end());
#endif
}

inline std::string WideToUtf8(std::wstring_view text) {
#if defined(_WIN32)
  if (text.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
#else
  return std::string(text.begin(), text.end());
#endif
}

}
