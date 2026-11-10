// core/elf_loader.h — loads the recompiled FlatOut 2 guest (fo2.aarch64) into this process.
#pragma once

#include <cstdint>
#include <cstddef>
#include <elf.h>
#include <vector>

namespace fo2 {

struct GuestImage {
    uint64_t entry = 0;          // e_entry (+ slide delta)
    uint64_t phdr_vaddr = 0;     // mapped program headers (for AT_PHDR)
    uint64_t phnum = 0;
    uint64_t base_vaddr = 0;     // first PT_LOAD p_vaddr as mapped (0x1000000 + delta)
    uint64_t end_vaddr = 0;
    uint64_t slide_delta = 0;    // how far the image moved from the link-time base
                                 // (0 = loaded at 0x1000000; > 0 = slid to a free hole)

    // Link-time program headers (NOT rebased): dl_iterate_phdr reports them with
    // dlpi_addr = slide_delta so the guest unwinder computes real addresses.
    std::vector<Elf64_Phdr> phdrs;

    // TLS segment initial image (PT_TLS), already zero-extended to memsz.
    uint8_t* tls_image = nullptr;
    size_t   tls_filesz = 0;
    size_t   tls_memsz = 0;
    size_t   tls_align = 1;

    void (*init_array)(void) = nullptr;   // first entry of DT_INIT_ARRAY
    size_t   init_array_count = 0;
    void**   init_array_ptr = nullptr;
    void*    fini_array_ptr = nullptr;
    size_t   fini_array_count = 0;
};

// Maps the ELF at the fixed addresses, applies the GOT relocations and fills `img`.
// Guest code is NOT executed here; call guest_entry_jump() afterwards.
bool elf_load(const char* path, GuestImage& img);

} // namespace fo2
