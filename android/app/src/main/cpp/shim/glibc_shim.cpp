// shim/glibc_shim.cpp — shared shim state: stdio data symbols, exit plumbing.
#include "../core/shim_common.h"
#include "../core/guest_thread.h"

#include <stdio.h>

// The guest imports `stdout`/`stderr` as data symbols (GLOB_DAT). We hand it the
// address of these variables, which hold REAL bionic FILE* handles; every stdio
// shim/thunk then operates on the same bionic FILE objects consistently.
FILE* guest_stdout = nullptr;
FILE* guest_stderr = nullptr;

__attribute__((constructor(101))) static void init_stdio_aliases() {
    guest_stdout = stdout;
    guest_stderr = stderr;
}
