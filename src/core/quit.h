// core/quit.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

namespace eot {

[[noreturn]] void QuitProcess(int code = 0);

[[noreturn]] void RestartProcess();

}
