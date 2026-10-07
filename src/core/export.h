// core/export.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#if defined(_WIN32)
#define EOT_EXPORT __declspec(dllexport)
#else
#define EOT_EXPORT __attribute__((visibility("default")))
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace eot {

inline void *HostEntryPoint(const char *name) {
#if defined(_WIN32)
  if (HMODULE host = ::GetModuleHandleW(nullptr))
    return reinterpret_cast<void *>(::GetProcAddress(host, name));
  return nullptr;
#else
  return ::dlsym(RTLD_DEFAULT, name);
#endif
}

}
