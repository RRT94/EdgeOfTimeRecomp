// core/quit.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "core/quit.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <rex/logging.h>

#include "core/export.h"
#include "core/logging.h"
#include "gpu/device.h"
#include "platform/process.h"

namespace eot {

namespace {

constexpr unsigned kQuitDeadlineMs = 3000;

std::atomic<const char *> g_phase{"start"};
std::atomic<bool> g_quitting{false};

[[noreturn]] void EndProcess(int code) {
  rex::FlushLogging();
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
  for (;;)
    ::Sleep(1000);
#else
  std::_Exit(code);
#endif
}

}

namespace {

[[noreturn]] void EndAfterParking(int code, bool relaunch) {
  if (g_quitting.exchange(true, std::memory_order_acq_rel)) {
    for (;;)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EOT_INFO("[quit] shutting down");
  std::thread([code] {
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuitDeadlineMs));
    EOT_WARN("[quit] stalled in '{}' after {} ms; terminating anyway",
             g_phase.load(std::memory_order_relaxed), kQuitDeadlineMs);
    EndProcess(code);
  }).detach();

  g_phase.store("renderer", std::memory_order_relaxed);
  gpu::Video::BeginShutdown();
  if (relaunch) {
    g_phase.store("relaunch", std::memory_order_relaxed);
    if (!platform::RelaunchSelf())
      EOT_WARN("[quit] the restart could not start a new copy; quitting instead");
  }
  g_phase.store("exit", std::memory_order_relaxed);
  EndProcess(code);
}

}

void QuitProcess(int code) { EndAfterParking(code, false); }

void RestartProcess() {
  EOT_INFO("[quit] restarting");
  EndAfterParking(0, true);
}

}

extern "C" EOT_EXPORT void eot_quit_process(int code) { eot::QuitProcess(code); }
extern "C" EOT_EXPORT void eot_restart_process() { eot::RestartProcess(); }
