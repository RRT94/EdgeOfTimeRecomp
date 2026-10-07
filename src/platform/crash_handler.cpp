// platform/crash_handler.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "platform/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/types.h>
#include <rex/version.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "platform/fatal_dialog.h"

#if defined(_WIN32)
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#endif

namespace eot::platform {
namespace {

constexpr u64 kGuestAddressSpaceSize = 0x100000000ull;

constexpr u64 kHostImageSpan = 0x20000000ull;

#if defined(_WIN32)
u64 HostModuleBase() { return reinterpret_cast<u64>(&__ImageBase); }
#else
u64 HostModuleBase() {
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void *>(&HostModuleBase), &info) && info.dli_fbase)
    return reinterpret_cast<u64>(info.dli_fbase);
  return 0;
}
#endif

std::string g_host_module_name;
bool g_crash_handler_installed = false;

const char *HostModuleName() { return g_host_module_name.empty() ? "host" : g_host_module_name.c_str(); }

std::atomic_flag s_reporting = ATOMIC_FLAG_INIT;

const char *ExceptionCodeName(rex::arch::Exception::Code code) {
  using Code = rex::arch::Exception::Code;
  switch (code) {
  case Code::kAccessViolation:
    return "ACCESS_VIOLATION";
  case Code::kIllegalInstruction:
    return "ILLEGAL_INSTRUCTION";
  default:
    return "UNKNOWN";
  }
}

const char *AvOperationName(rex::arch::Exception::AccessViolationOperation op) {
  using Op = rex::arch::Exception::AccessViolationOperation;
  switch (op) {
  case Op::kRead:
    return "read";
  case Op::kWrite:
    return "write";
  default:
    return "unknown";
  }
}

bool InModule(u64 address, u64 base) { return base && address >= base && address - base < kHostImageSpan; }

std::string ForeignModuleAt(u64 address) {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module)
    return {};
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
  if (!n)
    return fmt::format("{:#018x}+{:#x}", reinterpret_cast<u64>(module), address - reinterpret_cast<u64>(module));
  const wchar_t *name = path;
  for (const wchar_t *p = path; *p; ++p)
    if (*p == L'\\' || *p == L'/')
      name = p + 1;
  std::string narrow;
  for (const wchar_t *p = name; *p; ++p)
    narrow.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
  return fmt::format("{}+{:#x}", narrow, address - reinterpret_cast<u64>(module));
#else
  Dl_info info{};
  if (!::dladdr(reinterpret_cast<const void *>(address), &info) || !info.dli_fbase)
    return {};
  const char *name = info.dli_fname ? info.dli_fname : "";
  for (const char *p = name; *p; ++p)
    if (*p == '/')
      name = p + 1;
  if (!*name)
    return fmt::format("{:#018x}+{:#x}", reinterpret_cast<u64>(info.dli_fbase),
                       address - reinterpret_cast<u64>(info.dli_fbase));
  return fmt::format("{}+{:#x}", name, address - reinterpret_cast<u64>(info.dli_fbase));
#endif
}

void LogBacktrace(u64 base) {
#if defined(_WIN32)
  void *frames[32] = {};
  const USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  if (!n)
    return;
  EOT_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  for (USHORT i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (InModule(a, base))
      EOT_CRITICAL("    [{:>2}] {:#018x}  ({}+{:#010x})", i, a, HostModuleName(), a - base);
    else
      EOT_CRITICAL("    [{:>2}] {:#018x}  ({})", i, a, ForeignModuleAt(a));
  }
#else
  void *frames[32] = {};
  const int n = ::backtrace(frames, 32);
  if (n <= 0)
    return;
  EOT_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  for (int i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (InModule(a, base))
      EOT_CRITICAL("    [{:>2}] {:#018x}  ({}+{:#010x})", i, a, HostModuleName(), a - base);
    else
      EOT_CRITICAL("    [{:>2}] {:#018x}  ({})", i, a, ForeignModuleAt(a));
  }
#endif
}

void LogStackCodeAddresses(const rex::arch::HostThreadContext *ctx, u64 base) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  const u64 sp = ctx->int_registers[4];
  if (!sp || (sp % sizeof(u64)) != 0)
    return;
  constexpr u64 kPageSize = 4096;
  const u64 limit = std::min(sp + 64 * sizeof(u64), (sp | (kPageSize - 1)) + 1);
  EOT_CRITICAL("stack code addresses (sp {:#018x}, first is the return address of the call that faulted):", sp);
  u32 found = 0;
  for (u64 p = sp; p + sizeof(u64) <= limit; p += sizeof(u64)) {
    u64 v = 0;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof(v));
    if (!InModule(v, base))
      continue;
    EOT_CRITICAL("    [sp+{:#05x}] {:#018x}  ({}+{:#010x})", p - sp, v, HostModuleName(), v - base);
    if (++found >= 24)
      break;
  }
  if (!found)
    EOT_CRITICAL("    (no module addresses on the stack)");
#else
  (void)base;
#endif
}

void LogRegisters(const rex::arch::HostThreadContext *ctx) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  static const char *const kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  EOT_CRITICAL("    rip = {:#018x}   eflags = {:#010x}", ctx->rip, ctx->eflags);
  for (int i = 0; i < 16; ++i)
    EOT_CRITICAL("    {:>3} = {:#018x}", kNames[i], ctx->int_registers[i]);
#endif
}

bool CrashHandler(rex::arch::Exception *ex, void *) {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    return false;

  const u64 host_base = HostModuleBase();
  EOT_CRITICAL("================ reeot host crash ================");
  EOT_CRITICAL("build: reeot " REEOT_VERSION_STRING " " REEOT_GIT_COMMIT " " REXGLUE_BUILD_TITLE);
  EOT_CRITICAL("module base: {} @ {:#018x}", HostModuleName(), host_base);
  EOT_CRITICAL("exception: {} @ host pc {:#018x}", ExceptionCodeName(ex->code()), ex->pc());
  if (InModule(ex->pc(), host_base))
    EOT_CRITICAL("faulting RVA: {}+{:#010x}", HostModuleName(), ex->pc() - host_base);
  else
    EOT_CRITICAL("faulting module: {}", ForeignModuleAt(ex->pc()));

  if (ex->code() == rex::arch::Exception::Code::kAccessViolation) {
    const u64 fa = ex->fault_address();
    EOT_CRITICAL("fault address: {:#018x} ({})", fa, AvOperationName(ex->access_violation_operation()));
    auto *rt = rex::Runtime::instance();
    u8 *membase = rt ? rt->virtual_membase() : nullptr;
    const u64 base = reinterpret_cast<u64>(membase);
    if (membase && fa >= base && fa < base + kGuestAddressSpaceSize)
      EOT_CRITICAL("guest fault VA: {:#010x}", static_cast<u32>(fa - base));
  }

  EOT_CRITICAL("registers:");
  LogRegisters(ex->thread_context());
  LogStackCodeAddresses(ex->thread_context(), host_base);
  rex::FlushLogging();
  LogBacktrace(host_base);
  EOT_CRITICAL("===================================================");
  rex::FlushLogging();

  const std::string where = InModule(ex->pc(), host_base)
                                ? fmt::format("{}+{:#010x}", HostModuleName(), ex->pc() - host_base)
                                : fmt::format("{:#018x}", ex->pc());
  ShowFatalError("reeot crashed",
                 fmt::format("reeot hit a fatal error and has to close.\n\n{} at {}", ExceptionCodeName(ex->code()),
                             where));
#if !defined(_WIN32)
  std::_Exit(3);
#endif
  return false;
}

[[noreturn]] void TerminateHandler() {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    std::_Exit(3);

  std::string detail = "std::terminate with no active exception";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &e) {
      detail = std::string("uncaught ") + typeid(e).name() + ": " + e.what();
    } catch (...) {
      detail = "uncaught exception of a non-std type";
    }
  }

  EOT_CRITICAL("================ reeot host crash ================");
  EOT_CRITICAL("build: reeot " REEOT_VERSION_STRING " " REEOT_GIT_COMMIT " " REXGLUE_BUILD_TITLE);
  EOT_CRITICAL("{}", detail);
  LogBacktrace(HostModuleBase());
  EOT_CRITICAL("===================================================");
  rex::FlushLogging();
  ShowFatalError("reeot crashed", "reeot hit a fatal error and has to close.\n\n" + detail);
  std::abort();
}

void AbortHandler(int) {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    std::_Exit(3);
  EOT_CRITICAL("================ reeot host crash ================");
  EOT_CRITICAL("build: reeot " REEOT_VERSION_STRING " " REEOT_GIT_COMMIT " " REXGLUE_BUILD_TITLE);
  EOT_CRITICAL("abort() called (a CRT assert, an invalid parameter, or a library giving up)");
  LogBacktrace(HostModuleBase());
  EOT_CRITICAL("===================================================");
  rex::FlushLogging();
  ShowFatalError("reeot crashed", "reeot hit a fatal error and has to close.\n\nabort() was called; the log has the stack.");
  std::_Exit(3);
}

#if defined(_WIN32)
void InvalidParameterHandler(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned line,
                             uintptr_t) {
  std::string what = "a CRT function was given an invalid parameter";
  if (function || file) {
    what += " (";
    for (const wchar_t *p = function ? function : L""; *p; ++p)
      what.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    what += " at ";
    for (const wchar_t *p = file ? file : L""; *p; ++p)
      what.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    what += ":" + std::to_string(line) + ")";
  }
  (void)expression;
  if (!s_reporting.test_and_set(std::memory_order_acq_rel)) {
    EOT_CRITICAL("================ reeot host crash ================");
    EOT_CRITICAL("build: reeot " REEOT_VERSION_STRING " " REEOT_GIT_COMMIT " " REXGLUE_BUILD_TITLE);
    EOT_CRITICAL("{}", what);
    LogBacktrace(HostModuleBase());
    EOT_CRITICAL("===================================================");
    rex::FlushLogging();
    ShowFatalError("reeot crashed", "reeot hit a fatal error and has to close.\n\n" + what);
  }
  std::_Exit(3);
}
#else
void InstallAlternateSignalStack() {
  constexpr size_t kAltStackSize = 128 * 1024;
  static thread_local void *s_alt_stack = nullptr;
  if (s_alt_stack)
    return;
  s_alt_stack = std::malloc(kAltStackSize);
  if (!s_alt_stack)
    return;
  stack_t ss{};
  ss.ss_sp = s_alt_stack;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  ::sigaltstack(&ss, nullptr);
}
#endif

}

void InstallTerminateHandler() {
  if (g_host_module_name.empty())
    g_host_module_name = rex::filesystem::GetExecutablePath().filename().string();
  std::set_terminate(&TerminateHandler);
#if defined(_WIN32)
  std::signal(SIGABRT, &AbortHandler);
  _set_invalid_parameter_handler(&InvalidParameterHandler);
#else
  InstallAlternateSignalStack();
  struct sigaction sa{};
  sa.sa_handler = &AbortHandler;
  sa.sa_flags = SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGABRT, &sa, nullptr);
#endif
}

void InstallCrashHandler() {
  InstallTerminateHandler();
  if (g_crash_handler_installed)
    return;
  rex::arch::ExceptionHandler::Install(&CrashHandler, nullptr);
#if !defined(_WIN32)
  for (const int sig : {SIGSEGV, SIGILL, SIGBUS}) {
    struct sigaction current{};
    if (::sigaction(sig, nullptr, &current) == 0 && !(current.sa_flags & SA_ONSTACK)) {
      current.sa_flags |= SA_ONSTACK;
      ::sigaction(sig, &current, nullptr);
    }
  }
#endif
  g_crash_handler_installed = true;
  EOT_DEBUG("[crash] last-chance handler installed");
}

void UninstallCrashHandler() {
  if (!g_crash_handler_installed)
    return;
  rex::arch::ExceptionHandler::Uninstall(&CrashHandler, nullptr);
  g_crash_handler_installed = false;
}

}
