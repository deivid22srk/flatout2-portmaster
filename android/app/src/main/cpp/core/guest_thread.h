// core/guest_thread.h — guest thread / lifecycle internals shared across the runtime.
#pragma once

#include <cstdint>
#include <cstddef>
#include "shim_common.h"
#include "elf_loader.h"

namespace fo2 {

extern GuestImage g_guest;

typedef void* (*GuestThreadFn)(void*);
typedef void (*OnGuestExitFn)(int code);
extern OnGuestExitFn g_on_guest_exit;

void* tp_get();
void tp_set(void* tp);

// Switches TPIDR_EL0 to the thread's real bionic TCB while in scope.
// Safe to use on pure bionic threads (no-op) and guest threads (swap).
class BionicGuard {
public:
    BionicGuard();
    ~BionicGuard();
    BionicGuard(const BionicGuard&) = delete;
    BionicGuard& operator=(const BionicGuard&) = delete;
private:
    void* tp_;
    bool swapped_ = false;
};

// Creates a fresh TCB with a copy of the guest TLS image. TPIDR must be the
// thread's bionic value when called (stored into the TCB).
GuestTcb* guest_tcb_create();
void guest_tcb_destroy(GuestTcb* tcb);

// Runs fn on a new bionic pthread whose execution happens with a guest TCB.
bool run_guest_fn(GuestThreadFn fn, void* arg, size_t stack_size);

// [noreturn] Switches SP and jumps to the guest entry (core/entry_jump.S).
extern "C" void guest_entry_jump(void* entry, void* sp);

// Called by the exit shim when the guest main returns / calls exit().
void guest_notify_exit(int code);

} // namespace fo2
