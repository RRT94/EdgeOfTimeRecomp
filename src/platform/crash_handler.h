// platform/crash_handler.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

/**
 * @file    platform/crash_handler.h
 * @brief   Last-chance host crash reporters.
 *
 *          After reblue's platform/crash_handler (BSD 3-Clause, Tom Clay),
 *          the Windows part. A crash the process would otherwise die from in
 *          silence gets a log block (the faulting address as an offset into
 *          the executable the .pdb resolves, the registers, the stack) and a
 *          dialog.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

namespace eot::platform {

void InstallCrashHandler();
void UninstallCrashHandler();

void InstallTerminateHandler();

}
