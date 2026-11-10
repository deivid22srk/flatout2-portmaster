// core/guest_thread.cpp — guest TCB (glibc layout), TPIDR_EL0 switching and the
// guest main-thread startup.
//
// Every guest thread is a REAL bionic pthread, but while it runs guest code its
// TPIDR_EL0 points at a per-thread TCB laid out like glibc's, with the guest's
// PT_TLS image immediately before it. Shims switch TPIDR to the thread's real
// bionic TCB whenever they call into bionic (see the generated auto_thunks.S and
// BionicGuard here). This is what lets unmodified recompiled glibc code run on
// Android inside the app process.
#include "guest_thread.h"

#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace fo2 {

GuestImage g_guest{};
static bool g_running = false;
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
OnGuestExitFn g_on_guest_exit = nullptr;

// ---- TPIDR helpers -----------------------------------------------------------

void* tp_get() {
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return (void*)(uintptr_t)tp;
}

void tp_set(void* tp) {
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp) : "memory");
}

BionicGuard::BionicGuard() : tp_(tp_get()) {
    // Only swap when the current TPIDR really is one of our guest TCBs
    // (magic + self-pointer). On bionic threads this is a no-op.
    GuestTcb* tcb = (GuestTcb*)tp_;
    if (tp_ != nullptr && tcb->tcb == tp_ && tcb->reserved[0] == kTcbMagic &&
        tcb->bionic_tp != nullptr) {
        tp_set(tcb->bionic_tp);
        swapped_ = true;
    }
}

BionicGuard::~BionicGuard() {
    if (swapped_) tp_set(tp_);
}

// ---- guest TCB ---------------------------------------------------------------

GuestTcb* guest_tcb_create() {
    size_t tls_span = (g_guest.tls_memsz + 63u) & ~size_t(63); // keep TCB 64-aligned
    size_t total = tls_span + 4096;
    uint8_t* mem = (uint8_t*)calloc(1, total);
    if (mem == nullptr) return nullptr;
    if (g_guest.tls_image && g_guest.tls_filesz) memcpy(mem, g_guest.tls_image, g_guest.tls_filesz);
    GuestTcb* tcb = (GuestTcb*)(mem + tls_span);
    tcb->tcb = tcb;
    tcb->reserved[0] = kTcbMagic;
    static uint64_t dtv_dummy[2] = {0, 0};
    tcb->dtv = dtv_dummy;
    tcb->self = tcb;
    tcb->multiple_threads = 1;
    tcb->gscope_flag = 0;
    tcb->sysinfo = 0;
    static uint64_t canary = 0;
    if (canary == 0) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { (void)!read(fd, &canary, sizeof(canary)); close(fd); }
        if (canary == 0) canary = 0x466C61744F757432ULL; // "FlatOut2"
    }
    tcb->stack_guard = canary;
    tcb->bionic_tp = tp_get();
    tcb->owner = nullptr;
    return tcb;
}

void guest_tcb_destroy(GuestTcb* tcb) {
    if (tcb == nullptr) return;
    free(tcb);
}

// ---- guest threads -----------------------------------------------------------

namespace {

struct GuestBoot {
    GuestThreadFn fn;
    void* arg;
};

void* guest_trampoline(void* p) {
    GuestBoot boot = *reinterpret_cast<GuestBoot*>(p);
    free(p);

    GuestTcb* tcb = guest_tcb_create();
    if (tcb == nullptr) {
        loge("guest_tcb_create failed");
        return nullptr;
    }
    tcb->bionic_pthread = (void*)pthread_self();  // valid: still on bionic TPIDR here
    tp_set(tcb);
    void* result = boot.fn(boot.arg);
    tp_set(tcb->bionic_tp);
    guest_tcb_destroy(tcb);
    return result;
}

} // namespace

bool run_guest_fn(GuestThreadFn fn, void* arg, size_t stack_size) {
    GuestBoot* boot = (GuestBoot*)malloc(sizeof(GuestBoot));
    if (boot == nullptr) return false;
    boot->fn = fn;
    boot->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size > 0) pthread_attr_setstacksize(&attr, stack_size);

    pthread_t tid;
    int rc = pthread_create(&tid, &attr, guest_trampoline, boot);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        loge("pthread_create for guest thread failed: %d", rc);
        free(boot);
        return false;
    }
    pthread_detach(tid);
    return true;
}

// ---- guest main --------------------------------------------------------------

namespace {

// Builds the classic process startup stack:
//   [argc][argv...][NULL][envp...][NULL][auxv...][AT_NULL]   <- sp points here
//   [argv/env strings]                                        <- above, safe from pushes
void* build_initial_stack(size_t stack_size) {
    // 1. Collect the strings: argv = {argv0, --cap?}; envp = process environ.
    std::string cap;
    const char* cap_env = getenv("FO2_CAP");
    if (cap_env && *cap_env) cap = std::string("--cap=") + cap_env;

    std::vector<std::string> strs;          // argv0 .. argvN, then env strings
    std::vector<uint64_t> argv_pos;         // indices into strs
    strs.push_back("fo2.aarch64");
    argv_pos.push_back(0);
    if (!cap.empty()) { argv_pos.push_back(strs.size()); strs.push_back(cap); }

    std::vector<uint64_t> env_pos;          // indices into strs
    for (char** e = environ; e && *e; ++e) { env_pos.push_back(strs.size()); strs.push_back(*e); }

    size_t str_total = 0;
    for (const auto& s : strs) str_total += s.size() + 1;
    str_total = (str_total + 15) & ~size_t(15);

    // 2. Argument block layout (placeholder pointers, patched after mapping).
    std::vector<uint64_t> v;
    v.push_back(argv_pos.size());                    // argc
    for (size_t i = 0; i < argv_pos.size(); ++i) v.push_back(0);
    v.push_back(0);                                  // argv NULL
    for (size_t i = 0; i < env_pos.size(); ++i) v.push_back(0);
    v.push_back(0);                                  // envp NULL

    static uint64_t random_bytes[2] = {0x464C61744F757432ULL, 0x41494F706F7274ULL};
    auto push_aux = [&v](uint64_t k, uint64_t val) { v.push_back(k); v.push_back(val); };
    push_aux(AT_PHDR, g_guest.phdr_vaddr);
    push_aux(AT_PHENT, sizeof(Elf64_Phdr));
    push_aux(AT_PHNUM, g_guest.phnum);
    push_aux(AT_PAGESZ, (uint64_t)sysconf(_SC_PAGESIZE));
    push_aux(AT_BASE, 0);
    push_aux(AT_ENTRY, g_guest.entry);
    push_aux(AT_RANDOM, (uint64_t)(uintptr_t)random_bytes);
    push_aux(AT_CLKTCK, 100);
    push_aux(AT_SECURE, 0);
    push_aux(AT_UID, (uint64_t)getuid());
    push_aux(AT_EUID, (uint64_t)getuid());
    push_aux(AT_GID, (uint64_t)getgid());
    push_aux(AT_EGID, (uint64_t)getgid());
    push_aux(AT_HWCAP, getauxval(AT_HWCAP));
    push_aux(AT_EXECFN, 0); // patched to argv0 string
    push_aux(AT_NULL, 0);
    while (v.size() % 2 != 0) v.push_back(0); // keep sp 16-byte aligned

    size_t argblock = v.size() * 8;
    size_t region = str_total + argblock + stack_size;
    region = (region + 4095) & ~size_t(4095);

    uint8_t* base = (uint8_t*)mmap(nullptr, region, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        loge("initial stack mmap failed");
        return nullptr;
    }

    // 3. Strings at the very top (stack grows down, away from them).
    std::vector<uint64_t> str_addrs(strs.size(), 0);
    uint8_t* scursor = base + region;
    for (size_t i = 0; i < strs.size(); ++i) {
        scursor -= strs[i].size() + 1;
        memcpy(scursor, strs[i].data(), strs[i].size() + 1);
        str_addrs[i] = (uint64_t)(uintptr_t)scursor;
    }
    scursor = (uint8_t*)((uintptr_t)scursor & ~uint64_t(15));

    // 4. Patch argv/envp/AT_EXECFN pointers.
    size_t vi = 1;
    for (uint64_t p : argv_pos) v[vi++] = str_addrs[p];
    vi++;                                            // argv NULL
    for (uint64_t p : env_pos) v[vi++] = str_addrs[p];
    vi++;                                            // envp NULL
    v[v.size() - 2] = str_addrs[0];                  // AT_EXECFN

    uint8_t* sp = scursor - argblock;
    memcpy(sp, v.data(), argblock);
    return sp;
}

void* guest_main(void*) {
    GuestTcb* tcb = (GuestTcb*)tp_get();  // created by guest_trampoline

    // Run the guest's constructors (glibc calls INIT_ARRAY before main).
    if (g_guest.init_array_ptr && g_guest.init_array_count) {
        for (size_t i = 0; i < g_guest.init_array_count; ++i) {
            void (*fn)(void) = (void (*)(void))g_guest.init_array_ptr[i];
            if (fn) fn();
        }
    }

    void* sp = build_initial_stack(16 * 1024 * 1024);
    if (sp == nullptr) {
        tp_set(tcb->bionic_tp);
        guest_notify_exit(-1);
        return nullptr;
    }

    logi("jumping to guest entry 0x%llx", (unsigned long long)g_guest.entry);
    guest_entry_jump((void*)(uintptr_t)g_guest.entry, sp);
    return nullptr;  // not reached
}

} // namespace

void guest_notify_exit(int code) {
    pthread_mutex_lock(&g_state_mu);
    g_running = false;
    pthread_mutex_unlock(&g_state_mu);
    logi("guest exited with code %d", code);
    if (g_on_guest_exit) g_on_guest_exit(code);
}

bool guest_start(const char* binPath, const char*, int, int, int) {
    pthread_mutex_lock(&g_state_mu);
    bool already = g_running;
    pthread_mutex_unlock(&g_state_mu);
    if (already) return true;

    if (!elf_load(binPath, g_guest)) return false;

    pthread_mutex_lock(&g_state_mu);
    g_running = true;
    pthread_mutex_unlock(&g_state_mu);

    if (!run_guest_fn(guest_main, nullptr, 16 * 1024 * 1024)) {
        g_running = false;
        return false;
    }
    return true;
}

void guest_request_stop() {
    // The guest owns its own main loop; we don't force-kill threads. The SDL
    // shim's pause gate freezes rendering/audio; the process is torn down when
    // the activity is destroyed (the app does not go back to a sane menu).
}

bool guest_is_running() {
    pthread_mutex_lock(&g_state_mu);
    bool r = g_running;
    pthread_mutex_unlock(&g_state_mu);
    return r;
}

} // namespace fo2
