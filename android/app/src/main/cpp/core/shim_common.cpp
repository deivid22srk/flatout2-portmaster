// core/shim_common.cpp — logging, symbol registry lookup, shared state.
#include "shim_common.h"

#include <android/log.h>
#include <dlfcn.h>
#include <cstdarg>

#include "../shim/auto_imports.h"

namespace fo2 {

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
