#!/usr/bin/env python3
"""Generates the guest-import table and generic bionic thunks for the FlatOut 2
Android runtime, from the ACTUAL import list of fo2.aarch64.

Usage:  python3 gen_imports.py /path/to/fo2.aarch64
Writes: shim/auto_imports.h, shim/auto_imports.cpp, shim/auto_thunks.S
(These files are committed; CI does not need readelf or python.)

Classes:
  DlSym  - resolved at load time with dlsym(RTLD_DEFAULT): pure functions whose
           bionic implementations never touch thread-local state (string/mem,
           libm, thin syscall wrappers).
  Thunk  - generated aarch64 asm: switch TPIDR_EL0 to the thread's bionic TCB,
           call the bionic function of the same name, switch back. Used for
           everything that may malloc/pthread/use TLS inside bionic.
  Manual - hand-written shims (guest_<name>) for symbols whose semantics or
           names differ from bionic (see shim/glibc_shim_manual.cpp).
  Sdl    - our SDL2 shim (sdl_<name> in shim/sdl2_*.cpp).
  Weak0  - weak undefined imports, resolved to NULL exactly like glibc does.
"""

import re
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "shim"

# Hand-written shims (glibc_shim*.cpp). Each name must have a matching
# `extern "C" <ret> guest_<name>(...)` definition somewhere in the shim files.
MANUAL = {
    "__libc_start_main",
    # ctype tables in glibc format (bionic has different ones)
    "__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc",
    # locale-bound *_l functions: forward to the plain versions (C locale)
    "strcoll_l", "strtod_l", "strtof_l", "strtold_l", "strftime_l",
    "wcsxfrm_l", "wcscoll_l",
    "iswalpha_l", "iswblank_l", "iswcntrl_l", "iswdigit_l", "iswlower_l",
    "iswprint_l", "iswpunct_l", "iswspace_l", "iswupper_l", "iswxdigit_l",
    "tolower_l", "toupper_l", "towlower_l", "towupper_l",
    # glibc stat entry points (bionic exports plain stat/lstat/fstat)
    "__xstat", "__lxstat", "__fxstat",
    # dlopen needs name mapping (libEGL.so.1 -> libEGL.so, FFmpeg probe, ...)
    "dlopen",
    # timers: bionic lacks timer_create before API 28; use our own fallback
    "timer_create", "timer_settime",
    # stdio data symbols
    "stdout", "stderr",
    # pthread pieces that need guest-TCB <-> bionic pthread mapping
    "pthread_create", "pthread_self", "pthread_join", "pthread_detach",
    # bionic lacks these before API 28 / at all:
    "aligned_alloc",      # -> memalign
    "copy_file_range",    # -> raw syscall
    "backtrace", "backtrace_symbols_fd",  # stubs (crash log path only)
    "catopen", "catgets", "catclose",     # absent in bionic: stubs
    "__libc_current_sigrtmin",            # constant
    "__libc_single_threaded",             # data char (glibc 2.32+)
    # dl_iterate_phdr must also report the guest image: the guest's C++ unwinder
    # (static libc++abi/libunwind) finds its .eh_frame only through it, and bionic
    # knows nothing about the image we map in-process.
    "dl_iterate_phdr",
    # mmap: GuestMemory (disassembly-confirmed) calls
    # mmap(addr = 0x1000000000, len = 4 GiB, PROT_NONE, PRIVATE|ANON|NORESERVE)
    # and requires the EXACT base 0x1000000000 (recompiled code hardcodes
    # 0x1000000000 | guest_va in every memory access); a hinted mmap is not
    # honored when Android's JVM/GPU mappings cover the span.
    "mmap",
    # _Exit: the guest's fatal-error paths (fo2::terminate and friends) print to
    # stdout/stderr and then call _Exit — log it before the process disappears,
    # otherwise Android builds die with no trace in the logcat.
    "_Exit",
    # mprotect: GuestMemory::commit's mprotect failed on device (v1.0.0-android.4,
    # Android 14 / SD888) for the boot worker's fresh 4 GiB window while the main
    # thread's identical call succeeded, with errno invisible to the guest.
    # guest_mprotect (glibc_shim_manual.cpp) logs errno + overlapping maps and
    # falls back to an equivalent remap for ranges inside the flat guest window.
    "mprotect",
}

# Weak undefined imports: resolve to NULL (same as on glibc consoles).
WEAK = {
    "_ZTH19g_glesDeferBufferGL", "_ZTH20g_glesDeferTextureGL",
    "compat_newer_libm", "__gcov_dump", "__llvm_profile_write_file",
}

# Truly leaf/pure bionic functions: direct dlsym, no TPIDR switch needed.
PURE = {
    "memchr", "strchr", "strcmp", "strcpy", "strncmp", "strrchr", "strstr",
    "acos", "asin", "atan2", "cosh", "sinh", "tanh", "pow", "powf",
    "frexp", "ldexp", "modf", "lround", "lroundf",
    "nanosleep", "usleep", "clock_gettime", "time", "timegm",
    "gmtime_r", "localtime_r", "getpid", "sched_yield", "sysconf", "getenv",
    "read", "write", "open", "close", "lseek", "pread", "pwrite", "dup",
    "unlink", "unlinkat", "link", "symlink", "readlink", "mkdir", "rmdir",
    "chdir", "access", "truncate", "fcntl", "fsync", "ftruncate",
    "munmap", "madvise",
    "sendfile", "poll", "socket", "connect", "bind", "listen", "accept4",
    "send", "recv", "sendto", "recvfrom", "getsockname", "getpeername",
    "getsockopt", "setsockopt", "shutdown", "inet_ntop",
    "syscall",
}

# bionic target renames for thunks (guest name -> bionic symbol)
THUNK_RENAME = {
    "__isoc99_sscanf": "sscanf",
    "__isoc99_fscanf": "fscanf",
    "longjmp": "longjmp",
    "__errno_location": "__errno",  # bionic exports __errno, not __errno_location
}


def guest_imports(binpath: str):
    out = subprocess.check_output(
        ["readelf", "--dyn-syms", "-W", binpath], text=True)
    names = set()
    for line in out.splitlines():
        m = re.match(r"^\s*\d+:\s+[0-9a-f]+\s+\S+\s+\S+\s+\S+\s+\S+\s+UND\s+(\S+)", line)
        if m:
            names.add(m.group(1).split("@")[0])
    return sorted(names)


def classify(name: str) -> str:
    if name.startswith("SDL_"):
        return "Sdl"
    if re.match(r"^gl[A-Z]", name):
        return "DlSym"
    if name in WEAK:
        return "Weak0"
    if name in MANUAL:
        return "Manual"
    if name in PURE:
        return "DlSym"
    return "Thunk"


def cident(name: str) -> str:
    return re.sub(r"\W", "_", name)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    names = guest_imports(sys.argv[1])
    rows = [(n, classify(n)) for n in names]

    # ---- auto_imports.h -----------------------------------------------------
    hdr = ["// GENERATED by cpp/gen_imports.py - do not edit by hand.",
           "#pragma once", "#include <cstdint>", "#include <cstddef>", "",
           "namespace fo2 {", "",
           "enum class SymKind : uint8_t {",
           "    NotFound = 0, Thunk, Manual, Sdl, DlSym, Weak0,", "};", "",
           "struct ImportEntry { const char* name; SymKind kind; void* address; };",
           "const ImportEntry* find_import(const char* name);",
           "", "} // namespace fo2", ""]
    (OUT / "auto_imports.h").write_text("\n".join(hdr))

    # ---- auto_imports.cpp ---------------------------------------------------
    cpp = ["// GENERATED by cpp/gen_imports.py - do not edit by hand.",
           "// Address table for every dynamic import of fo2.aarch64.",
           '#include "auto_imports.h"',
           '#include <cstring>', "",
           "namespace fo2 {", "",
           "// Thunks live in auto_thunks.S; manual shims in glibc_shim*.cpp; SDL in sdl2_*.cpp.",
           'extern "C" {']
    for n, k in rows:
        if k == "Thunk":
            cpp.append(f"void fo2_thunk_{cident(n)}();")
        elif k == "Manual":
            cpp.append(f"void guest_{cident(n)}();")
        elif k == "Sdl":
            cpp.append(f"void sdl_{cident(n)}();")
    cpp.append("}")
    cpp.append("")
    cpp.append("static const ImportEntry kImports[] = {")
    for n, k in rows:
        addr = "nullptr"
        if k == "Thunk":
            addr = f"(void*)&fo2_thunk_{cident(n)}"
        elif k == "Manual":
            addr = f"(void*)&guest_{cident(n)}"
        elif k == "Sdl":
            addr = f"(void*)&sdl_{cident(n)}"
        cpp.append(f'    {{"{n}", SymKind::{k}, {addr}}},')
    cpp.append("};")
    cpp.append("""
const ImportEntry* find_import(const char* name) {
    for (const ImportEntry& e : kImports)
        if (std::strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

} // namespace fo2
""")
    (OUT / "auto_imports.cpp").write_text("\n".join(cpp))

    # ---- auto_thunks.S ------------------------------------------------------
    s = ["// GENERATED by cpp/gen_imports.py - do not edit by hand.",
         "// Generic guest->bionic forwarding thunks (aarch64).",
         "//",
         "// Guest threads run with TPIDR_EL0 = guest TCB (glibc layout). These thunks",
         "// swap in the thread's REAL bionic TCB for the duration of the bionic call.",
         "// SP is never touched, so variadic calls (printf family) are safe;",
         "// x19/x30 are saved in the guest TCB's private area, argument and vector",
         "// registers pass through untouched.",
         ".text", "",
         ".macro FOTHUNK name, target",
         ".p2align 2",
         ".hidden \\name",
         ".globl \\name",
         ".type \\name, %function",
         "\\name:",
         "    mrs     x9, tpidr_el0",
         "    ldr     x10, [x9, #192]      // GuestTcb::bionic_tp (+0xC0)",
         "    str     x30, [x9, #216]      // private: saved LR (+0xD8)",
         "    str     x19, [x9, #224]      // private: saved x19 (+0xE0)",
         "    mov     x19, x9",
         "    msr     tpidr_el0, x10",
         "    bl      \\target",
         "    ldr     x30, [x19, #216]",
         "    msr     tpidr_el0, x19",
         "    ldr     x19, [x19, #224]",
         "    ret",
         ".size \\name, .-\\name",
         ".endm", ""]
    for n, k in rows:
        if k == "Thunk":
            target = THUNK_RENAME.get(n, n)
            s.append(f"FOTHUNK fo2_thunk_{cident(n)}, {target}")
    s.append("")
    (OUT / "auto_thunks.S").write_text("\n".join(s))

    kinds = {}
    for _, k in rows:
        kinds[k] = kinds.get(k, 0) + 1
    print(f"{len(rows)} imports: " + ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))


if __name__ == "__main__":
    main()
