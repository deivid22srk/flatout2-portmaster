// core/entry_jump.S — switches SP and jumps into the guest's _start. Never returns.
.text
.p2align 2
.globl guest_entry_jump
.hidden guest_entry_jump
.type guest_entry_jump, %function
// void guest_entry_jump(void* entry /*x0*/, void* sp /*x1*/)
guest_entry_jump:
    mov     sp, x1
    br      x0
.size guest_entry_jump, .-guest_entry_jump
