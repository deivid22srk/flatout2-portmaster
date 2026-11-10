// core/shim_common.cpp — logging, symbol registry lookup, shared state.
#include "shim_common.h"

#include <android/log.h>
#include <dlfcn.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#include "../shim/auto_imports.h"

namespace fo2 {

// ---- flat guest window (GuestMemory inside the guest) --------------------------

// The guest's GuestMemory ctor does: mmap(hint 0x100000000, 64 GiB, PROT_NONE,
// PRIVATE|ANON|NORESERVE) and throws unless the kernel returns EXACTLY
// 0x100000000 — recompiled code hardcodes (0x100000000 | guest_va) everywhere.
constexpr uint64_t kFlatBase = 0x100000000ull;   // 4 GiB
constexpr uint64_t kFlatSpan = 0x1000000000ull;  // 64 GiB

bool read_self_maps(std::vector<SelfMapsEntry>* out) {
    FILE* f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr) {
        uint64_t s = 0, e = 0;
        char perms[8] = {0};
        unsigned long long off = 0, ino = 0;
        unsigned int devmaj = 0, devmin = 0;
        int consumed = 0;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %llx %x:%x %llu %n",
                   &s, &e, perms, &off, &devmaj, &devmin, &ino, &consumed) < 7) {
            continue;
        }
        SelfMapsEntry m;
        m.start = s;
        m.end = e;
        m.perms = perms;
        m.inode = ino;
        const char* p = line + consumed;
        while (*p == ' ' || *p == '\t') ++p;
        char* nl = strchr(line, '\n');   // strip the trailing newline, if any
        if (nl != nullptr) *nl = '\0';
        m.path = p;
        out->push_back(std::move(m));
    }
    fclose(f);
    return !out->empty();
}

void log_overlapping_maps(uint64_t start, uint64_t end, const char* why) {
    std::vector<SelfMapsEntry> maps;
    if (!read_self_maps(&maps)) {
        loge("%s: cannot read /proc/self/maps", why);
        return;
    }
    int shown = 0;
    for (const auto& m : maps) {
        if (m.start < end && start < m.end) {
            loge("%s: overlap 0x%" PRIx64 "-0x%" PRIx64 " %s inode=%llu %s",
                 why, m.start, m.end, m.perms.c_str(), m.inode,
                 m.path.empty() ? "(anonymous)" : m.path.c_str());
            if (++shown >= 8) {
                loge("%s: ... more suppressed", why);
                break;
            }
        }
    }
}

int drop_pure_gap_maps(uint64_t start, uint64_t end) {
    std::vector<SelfMapsEntry> maps;
    if (!read_self_maps(&maps)) return 0;
    int dropped = 0;
    for (const auto& m : maps) {
        if (m.start < end && start < m.end && m.perms == "---p" &&
            m.inode == 0 && m.path.empty()) {
            uint64_t s = std::max(m.start, start), e = std::min(m.end, end);
            if (munmap((void*)(uintptr_t)s, e - s) == 0) {
                logi("dropped pure gap reservation 0x%" PRIx64 "-0x%" PRIx64, s, e);
                ++dropped;
            }
        }
    }
    return dropped;
}

void flat_memory_prepare() {
    // MAP_FIXED_NOREPLACE probe: succeeds only when the whole span is free, and
    // gives us the exact blockers when it is not.
    void* p = mmap((void*)(uintptr_t)kFlatBase, kFlatSpan, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, kFlatSpan);   // free again: the guest's own hinted mmap must take it
        logi("flat guest window 0x%" PRIx64 "-0x%" PRIx64 " is free",
             kFlatBase, kFlatBase + kFlatSpan);
        return;
    }
    if (errno != EEXIST) {
        loge("flat guest window probe failed: %s (errno %d)", strerror(errno), errno);
        return;
    }
    loge("flat guest window 0x%" PRIx64 "-0x%" PRIx64 " is taken; diagnosing",
         kFlatBase, kFlatBase + kFlatSpan);
    log_overlapping_maps(kFlatBase, kFlatBase + kFlatSpan, "flat window");
    if (drop_pure_gap_maps(kFlatBase, kFlatBase + kFlatSpan) > 0) {
        p = mmap((void*)(uintptr_t)kFlatBase, kFlatSpan, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                 -1, 0);
        if (p != MAP_FAILED) {
            munmap(p, kFlatSpan);
            logi("flat guest window free after dropping gap reservations");
            return;
        }
        loge("flat guest window still taken after dropping gaps (errno %d)", errno);
        log_overlapping_maps(kFlatBase, kFlatBase + kFlatSpan, "flat window");
    }
    // Real mappings block the span. guest_mmap (shim) reports this again when the
    // guest actually asks; nothing else we can safely do from here.
}

void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "FO2", fmt, ap);
    va_end(ap);
}

void loge(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, "FO2", fmt, ap);
    va_end(ap);
}

void* find_guest_symbol(const char* name) {
    // 1. Static import table (generated from the actual fo2.aarch64 import list).
    const ImportEntry* e = find_import(name);
    if (e == nullptr) {
        loge("guest import not in table: %s", name);
        return nullptr;
    }
    switch (e->kind) {
        case SymKind::Sdl:
        case SymKind::Manual:
        case SymKind::Thunk:
            return e->address;
        case SymKind::Weak0:
            return nullptr; // same as glibc: weak undefined resolves to 0
        case SymKind::DlSym: {
            void* p = dlsym(RTLD_DEFAULT, name);
            if (p == nullptr) loge("dlsym failed for guest import: %s", name);
            return p;
        }
        default:
            loge("unresolved import: %s", name);
            return nullptr;
    }
}

} // namespace fo2
