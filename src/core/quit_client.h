// core/quit_client.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdlib>

#include "core/export.h"

namespace eot {

[[noreturn]] inline void QuitProcessFromModule(int code = 0) {
  if (auto *quit = reinterpret_cast<void (*)(int)>(HostEntryPoint("eot_quit_process")))
    quit(code);
  std::_Exit(code);
}

[[noreturn]] inline void RestartProcessFromModule() {
  if (auto *restart = reinterpret_cast<void (*)()>(HostEntryPoint("eot_restart_process")))
    restart();
  QuitProcessFromModule(0);
}

}
