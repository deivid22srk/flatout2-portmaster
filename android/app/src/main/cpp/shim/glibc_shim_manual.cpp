// shim/glibc_shim_manual.cpp — hand-written shims for guest imports whose names
// or semantics differ from bionic. Everything here runs on a GUEST thread
// (TPIDR_EL0 = guest TCB) unless noted: any call into bionic must be inside a
// fo2::BionicGuard. Pure syscalls may be made directly.
#include "shim_common.h"
#include "../core/guest_thread.h"

#include <dlfcn.h>
#include <ctype.h>
#include <elf.h>
#include <atomic>
#include <cinttypes>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

using fo2::BionicGuard;
using fo2::logi;
using fo2::loge;
using fo2::log_overlapping_maps;
using fo2::drop_pure_gap_maps;
using fo2::tp_get;
using fo2::tp_set;

extern "C" {

// ---- process startup / exit --------------------------------------------------
//
// The guest's _start calls this (as glibc's). Runs on the guest main thread with
// the TCB already active and INIT_ARRAY already executed (see guest_main).
int guest___libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                            void (*init)(void), void (*fini)(void),
                            void (*rtld_fini)(void), void* stack_end) {
    (void)init;    // INIT_ARRAY ran before entry; glibc would call it here.
    (void)fini;
    (void)stack_end;
    char** envp = argv + argc + 1;

    int code = main(argc, argv, envp);

    if (rtld_fini) rtld_fini();
    fo2::guest_notify_exit(code);
    // Never return into the guest's _start: park this thread forever with the
    // bionic TPIDR active (the app decides what to show next).
    {
        BionicGuard g;
        pthread_exit(nullptr);
    }
    return code; // unreachable
}

// ---- dlopen with name mapping -------------------------------------------------
//
// The guest (and its D3D9->GLES presenter) probes Linux library sonames.
void* guest_dlopen(const char* file, int mode) {
    std::string mapped = file ? file : "";
    if (mapped == "libEGL.so.1") mapped = "libEGL.so";
    else if (mapped == "libGLESv2.so.2") mapped = "libGLESv2.so";
    else if (mapped == "libGLESv3.so.3") mapped = "libGLESv3.so";
    // FFmpeg sonames (libavcodec.so.5x...) pass through: they fail naturally
    // when not bundled, and the port treats missing FFmpeg as "no FMVs".

    BionicGuard g;
    void* h = dlopen(mapped.c_str(), mode);
    if (h == nullptr) logi("guest dlopen(%s) -> %s (failed: %s)",
                           file ? file : "(null)", mapped.c_str(), dlerror());
    return h;
}

// ---- stat family (glibc entry points over bionic stat) -------------------------
// aarch64 glibc and bionic share the kernel struct stat layout.
int guest___xstat(int ver, const char* path, struct stat* buf) {
    (void)ver;
    BionicGuard g;
    return stat(path, buf);
}
int guest___lxstat(int ver, const char* path, struct stat* buf) {
    (void)ver;
    BionicGuard g;
    return lstat(path, buf);
}
int guest___fxstat(int ver, int fd, struct stat* buf) {
    (void)ver;
    BionicGuard g;
    return fstat(fd, buf);
}

// ---- allocation: bionic lacks aligned_alloc before API 28 ----------------------
void* guest_aligned_alloc(size_t align, size_t size) {
    BionicGuard g;
    if (align < sizeof(void*)) align = sizeof(void*);
    return memalign(align, size);
}

// ---- _Exit: the guest's fatal paths (fo2::terminate etc.) end here -------------
//
// On Linux those paths print the real reason to stdout/stderr first; on Android
// both are silent (no console), so an unexplained _Exit here used to mean the
// app just "closes" with no log line at all. Flush and log before the process
// vanishes, then exit like the guest asked (cannot return).
void guest__Exit(int code) {
    BionicGuard g;
    fflush(nullptr);                       // drain stdout/stderr buffers into the pipe
    logi("guest _Exit(%d) — the reason, if any, is in the output above", code);
    usleep(150000);                        // let the output reader log what was flushed
    _Exit(code);
}

// ---- mmap: the flat guest address space must land at exactly 0x1000000000 ------
//
// GuestMemory (inside the guest, disassembly-confirmed) reserves 4 GiB of host
// address space with mmap(addr = 0x1000000000, len = 0x100000000, PROT_NONE,
// PRIVATE|ANON|NORESERVE) and FAILS unless the kernel returns the EXACT base
// 0x1000000000: the recompiled game hardcodes (0x1000000000 | guest_va) in
// every memory access. Its mmap is only a HINT (no MAP_FIXED), so on Android —
// where the JVM and the GPU driver map anywhere — the kernel may silently pick
// another address and the game aborts in testGuestMemory right after the
// window is created. Intercept that exact request and force the placement.
void* guest_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    constexpr uint64_t kFlatBase = 0x1000000000ull;  // 64 GiB
    constexpr uint64_t kFlatSpan = 0x100000000ull;   // 4 GiB

    const bool flat_request = (uint64_t)(uintptr_t)addr == kFlatBase && len == kFlatSpan &&
                              prot == PROT_NONE && fd == -1 &&
                              (flags & (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)) ==
                                  (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE) &&
                              (flags & MAP_FIXED) == 0;
    if (!flat_request) {
        // Everything else: plain syscall semantics (mmap itself touches no TLS).
        return mmap(addr, len, prot, flags, fd, off);
    }

    // Every creation is logged: testGuestMemory creates and frees one window
    // before the runtime builds its own, and the once-only "reported" gate hid
    // the second creation — the exact state we need when commits start failing.
    static std::atomic<int> creations{0};
    for (int attempt = 0; attempt < 2; ++attempt) {
        // If our own reservation covers the span, drop it first: MAP_FIXED_NOREPLACE
        // fails (EEXIST) even against a PROT_NONE anonymous mapping.
        if (fo2::flat_reserved_start() == kFlatBase &&
            fo2::flat_reserved_end() == kFlatBase + kFlatSpan) {
            munmap((void*)(uintptr_t)kFlatBase, kFlatSpan);
            fo2::flat_clear_reservation();
            logi("handed our flat-window reservation to the guest");
        }
        void* p = mmap((void*)(uintptr_t)kFlatBase, len, PROT_NONE,
                       flags | MAP_FIXED_NOREPLACE, fd, off);
        if (p != MAP_FAILED) {
            logi("flat guest window #%d reserved at 0x1000000000 (4 GiB)",
                 creations.fetch_add(1) + 1);
            return p;
        }
        int err = errno;
        loge("flat guest window mmap attempt %d failed: errno %d", attempt + 1, err);
        if (err != EEXIST) break;
        if (attempt == 0) {
            loge("flat guest window is taken; diagnosing");
            log_overlapping_maps(kFlatBase, kFlatBase + len, "flat window");
            if (drop_pure_gap_maps(kFlatBase, kFlatBase + len) == 0) break;
        }
    }

    // Still blocked: fall back to the plain hinted call. The guest throws its
    // own clear error ("Guest address space could not be reserved at the fixed
    // base") instead of failing mysteriously later.
    int err = errno;
    void* p = mmap(addr, len, prot, flags, fd, off);
    loge("flat guest window unavailable (errno %d); kernel gave %p -- the game "
         "will abort in testGuestMemory unless this is fixed", err, p);
    return p;
}

// ---- mprotect: GuestMemory::commit turns reserved window pages RW --------------
//
// On device (v1.0.0-android.4, motorola edge 30 fusion / Android 14): the main
// thread's testGuestMemory commits its ranges through this exact path fine, yet
// ~1 ms later the boot worker's commit throws "Guest commit failed" on the
// first mprotect of a fresh 4 GiB window. The guest discards errno, so the
// kernel's answer was invisible. Wrap it: log addr/len/prot + errno + the
// overlapping /proc/self/maps lines, and for ranges inside the flat guest
// window fall back to an equivalent munmap-free mmap(MAP_FIXED): commit only
// turns fresh PROT_NONE reservation pages accessible, and fresh anonymous
// pages are exactly that. Whatever eats the mapping (a hole nobody mapped, a
// THP split, a stray driver mapping) is healed — and now visible in the log.
int guest_mprotect(void* addr, size_t len, int prot) {
    int rc = mprotect(addr, len, prot);
    if (rc == 0) return 0;

    int err = errno;
    const uint64_t start = (uint64_t)(uintptr_t)addr, end = start + (uint64_t)len;
    loge("guest mprotect(0x%" PRIx64 ", %zu, %d) failed: errno %d (diagnosing)",
         start, len, prot, err);
    log_overlapping_maps(start, end, "mprotect fail");

    // Emulate the commit for guest-window pages only (the game's own reservation);
    // content-bearing mappings elsewhere must keep the real syscall's verdict.
    constexpr uint64_t kFlatBase = 0x1000000000ull;
    constexpr uint64_t kFlatSpan = 0x100000000ull;
    const bool page_aligned = (start & 0xfffull) == 0 && (len & 0xfffull) == 0;
    if (len && page_aligned && start >= kFlatBase && end <= kFlatBase + kFlatSpan) {
        void* p = mmap(addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == addr) {
            static std::atomic<int> remaps{0};
            if (remaps.fetch_add(1) < 8)
                logi("guest mprotect fallback: remapped 0x%" PRIx64 "-0x%" PRIx64 " (prot %d)",
                     start, end, prot);
            return 0;
        }
        loge("guest mprotect remap fallback failed: errno %d", errno);
    }
    return rc;
}

// ---- dl_iterate_phdr: report the guest image so C++ exceptions can unwind ------
//
// The guest carries a STATIC libc++abi/libunwind; its unwinder locates FDEs
// through dl_iterate_phdr. On Linux the main executable (this guest) is always
// in that list; on Android bionic knows nothing about the image we mapped
// in-process, so the first `throw` in guest code would fail to unwind and end
// in std::terminate/abort. We report the guest image first (dlpi_addr = slide
// delta, link-time phdrs: dlpi_addr + p_vaddr = the real mapped address), then
// the host's own modules.
namespace {

struct PhdrBridge {
    int (*fn)(struct dl_phdr_info*, size_t, void*);
    void* data;
    void* guest_tp;    // this thread's guest TCB
    void* bionic_tp;   // this thread's real bionic TCB
};

// bionic invokes the callback; the callback is guest code and needs the guest
// TCB back for its duration.
int phdr_bridge(struct dl_phdr_info* info, size_t size, void* v) {
    PhdrBridge* b = (PhdrBridge*)v;
    tp_set(b->guest_tp);
    int r = b->fn(info, size, b->data);
    tp_set(b->bionic_tp);   // bionic keeps iterating and holds its loader lock
    return r;
}

} // namespace

int guest_dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
    void* tp = tp_get();
    fo2::GuestTcb* tcb = (fo2::GuestTcb*)tp;
    const bool on_guest_tcb = tp != nullptr && tcb->tcb == tp &&
                              tcb->reserved[0] == fo2::kTcbMagic && tcb->bionic_tp != nullptr;

    // 1) The guest image — the only module whose frames guest exceptions cross.
    if (fo2::g_guest.phnum > 0 && !fo2::g_guest.phdrs.empty()) {
        struct dl_phdr_info guest_info{};
        guest_info.dlpi_addr = (ElfW(Addr))fo2::g_guest.slide_delta;
        guest_info.dlpi_name = "fo2.aarch64";
        guest_info.dlpi_phdr = fo2::g_guest.phdrs.data();
        guest_info.dlpi_phnum = (ElfW(Half))fo2::g_guest.phnum;
        int r = callback(&guest_info, sizeof(guest_info), data);
        if (r != 0) return r;
    }

    // 2) The host's own modules (harmless; helps foreign unwinders).
    {
        BionicGuard g;
        if (on_guest_tcb) {
            PhdrBridge bridge{callback, data, tp, tcb->bionic_tp};
            return dl_iterate_phdr(phdr_bridge, &bridge);
        }
        return dl_iterate_phdr(callback, data);
    }
}

// ---- copy_file_range: raw syscall (bionic wrapper exists only on API 34+) ------
ssize_t guest_copy_file_range(int fd_in, off64_t* off_in, int fd_out,
                              off64_t* off_out, size_t len, unsigned int flags) {
    return syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
}

// ---- execinfo: guest only uses these for crash logs; bionic availability varies -
int guest_backtrace(void**, int) { return 0; }
void guest_backtrace_symbols_fd(void*, int, int) {}

// ---- catgets: absent in bionic, guest falls back to its own strings ------------
void* guest_catopen(const char*, int) { return (void*)(uintptr_t)-1; }
char* guest_catgets(void*, int, int, const char* msg) { return (char*)msg; }
int guest_catclose(void*) { return -1; }

// ---- misc libc internals --------------------------------------------------------
int guest___libc_current_sigrtmin(void) { return 34; }  // glibc reserves 32/33
char guest___libc_single_threaded = 0;                  // glibc 2.32+ data symbol

// ---- locale-bound *_l functions: forward to plain versions (C locale) ----------
int guest_strcoll_l(const char* a, const char* b, void*) {
    BionicGuard g; return strcmp(a, b);
}
size_t guest_strxfrm_l(char* d, const char* s, size_t n, void*) {
    BionicGuard g; return strxfrm(d, s, n);
}
double guest_strtod_l(const char* s, char** e, void*) {
    BionicGuard g; return strtod(s, e);
}
float guest_strtof_l(const char* s, char** e, void*) {
    BionicGuard g; return strtof(s, e);
}
long double guest_strtold_l(const char* s, char** e, void*) {
    BionicGuard g; return strtold(s, e);
}
size_t guest_strftime_l(char* s, size_t max, const char* fmt, const struct tm* tm, void*) {
    BionicGuard g; return strftime(s, max, fmt, tm);
}
int guest_wcscoll_l(const wchar_t* a, const wchar_t* b, void*) {
    BionicGuard g; return wcscmp(a, b);
}
size_t guest_wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, void*) {
    BionicGuard g; return wcsxfrm(d, s, n);
}
int guest_iswalpha_l(wint_t c, void*) { return iswalpha(c); }
int guest_iswblank_l(wint_t c, void*) { return iswblank(c); }
int guest_iswcntrl_l(wint_t c, void*) { return iswcntrl(c); }
int guest_iswdigit_l(wint_t c, void*) { return iswdigit(c); }
int guest_iswlower_l(wint_t c, void*) { return iswlower(c); }
int guest_iswprint_l(wint_t c, void*) { return iswprint(c); }
int guest_iswpunct_l(wint_t c, void*) { return iswpunct(c); }
int guest_iswspace_l(wint_t c, void*) { return iswspace(c); }
int guest_iswupper_l(wint_t c, void*) { return iswupper(c); }
int guest_iswxdigit_l(wint_t c, void*) { return iswxdigit(c); }
wint_t guest_towlower_l(wint_t c, void*) { return towlower(c); }
wint_t guest_towupper_l(wint_t c, void*) { return towupper(c); }
int guest_tolower_l(int c, void*) { return tolower(c); }
int guest_toupper_l(int c, void*) { return toupper(c); }

// ---- pthread: guest handles are guest TCBs (like glibc, where pthread_t == TCB*) -
struct GuestPthreadBoot {
    void* (*start)(void*);
    void* arg;
    fo2::GuestTcb* tcb;
};

static void* guest_pthread_trampoline(void* p) {
    GuestPthreadBoot boot = *(GuestPthreadBoot*)p;
    free(p);

    fo2::GuestTcb* tcb = boot.tcb;
    tcb->bionic_tp = tp_get();              // this thread's real bionic TCB
    tcb->bionic_pthread = (void*)pthread_self();   // still on bionic TPIDR here
    fo2::tp_set(tcb);
    void* result = boot.start(boot.arg);
    // The TCB stays allocated on purpose: pthread_join still needs
    // bionic_pthread after the thread exits (guest may join later).
    return result;
}

int guest_pthread_create(void* thread_out, const void* attr,
                         void* (*start)(void*), void* arg) {
    GuestPthreadBoot* boot = (GuestPthreadBoot*)malloc(sizeof(GuestPthreadBoot));
    if (boot == nullptr) { errno = EAGAIN; return EAGAIN; }
    boot->start = start;
    boot->arg = arg;
    boot->tcb = fo2::guest_tcb_create();
    if (boot->tcb == nullptr) { free(boot); errno = EAGAIN; return EAGAIN; }

    int rc;
    {
        BionicGuard g;
        pthread_attr_t local_attr;
        pthread_attr_t* attr_ptr = nullptr;
        if (attr != nullptr) {
            // The guest's attr was written through our thunks, so its memory
            // already holds bionic-form values (stack_size shares offset 0x10).
            memcpy(&local_attr, attr, sizeof(local_attr));
            attr_ptr = &local_attr;
        } else {
            pthread_attr_init(&local_attr);
            pthread_attr_setstacksize(&local_attr, 8 * 1024 * 1024); // glibc default
            attr_ptr = &local_attr;
        }
        pthread_t tid;
        rc = pthread_create(&tid, attr_ptr, guest_pthread_trampoline, boot);
        if (rc == 0) boot->tcb->bionic_pthread = (void*)tid;
    }
    if (rc != 0) {
        fo2::guest_tcb_destroy(boot->tcb);
        free(boot);
        errno = rc;
        return rc;
    }

    // glibc exposes pthread_t as the TCB pointer; mirror that.
    *(void**)thread_out = boot->tcb;
    return 0;
}

void* guest_pthread_self(void) { return tp_get(); }

int guest_pthread_join(void* guest_thread, void** retval) {
    fo2::GuestTcb* tcb = (fo2::GuestTcb*)guest_thread;
    if (tcb == nullptr || tcb->bionic_pthread == nullptr) return ESRCH;
    BionicGuard g;
    return pthread_join((pthread_t)tcb->bionic_pthread, retval);
}

int guest_pthread_detach(void* guest_thread) {
    fo2::GuestTcb* tcb = (fo2::GuestTcb*)guest_thread;
    if (tcb == nullptr || tcb->bionic_pthread == nullptr) return ESRCH;
    BionicGuard g;
    return pthread_detach((pthread_t)tcb->bionic_pthread);
}

// ---- timers: glibc timer_create over timerfd -----------------------------------
namespace {
struct Fo2Timer {
    int fd;
    void (*cb)(void*);
    void* cb_arg;
};

void* timer_guest_loop(void* p) {
    Fo2Timer* t = (Fo2Timer*)p;
    uint64_t expirations;
    // Raw read on a timerfd: no bionic TLS involved, safe from guest context.
    while (::read(t->fd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations)) {
        if (t->cb) t->cb(t->cb_arg);  // guest code, guest TPIDR
    }
    return nullptr;
}
} // namespace

int guest_timer_create(int clockid, struct sigevent* sev, void** timerid) {
    if (sev == nullptr || sev->sigev_notify != SIGEV_THREAD) {
        errno = EINVAL;
        return -1;
    }
    int fd;
    {
        BionicGuard g;
        fd = timerfd_create(clockid, 0);
    }
    if (fd < 0) return -1;

    Fo2Timer* t = (Fo2Timer*)malloc(sizeof(Fo2Timer));
    if (t == nullptr) { close(fd); errno = ENOMEM; return -1; }
    t->fd = fd;
    t->cb = (void (*)(void*))sev->sigev_notify_function;
    t->cb_arg = (void*)sev->sigev_value.sival_ptr;

    // Run the dispatch loop on a guest-context thread so the callback sees the
    // guest TCB (glibc SIGEV_THREAD runs callbacks on fresh threads too).
    if (!fo2::run_guest_fn(timer_guest_loop, t, 1024 * 1024)) {
        close(fd);
        free(t);
        errno = EAGAIN;
        return -1;
    }
    *timerid = (void*)t;
    return 0;
}

int guest_timer_settime(void* timerid, int flags, const struct itimerspec* value,
                        struct itimerspec* old) {
    Fo2Timer* t = (Fo2Timer*)timerid;
    if (t == nullptr) { errno = EINVAL; return -1; }
    BionicGuard g;
    return timerfd_settime(t->fd, flags, value, old);
}

} // extern "C"
