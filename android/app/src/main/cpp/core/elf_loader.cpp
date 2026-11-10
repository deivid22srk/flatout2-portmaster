// core/elf_loader.cpp — in-process loader for the recompiled FlatOut 2 guest.
//
// fo2.aarch64 is an ET_EXEC (non-PIE) AArch64 glibc binary at a fixed base
// (0x1000000) with BIND_NOW. Its dynamic surface, verified with readelf:
//   * 370 JUMP_SLOT + 8 GLOB_DAT relocations — the only reloc kinds present
//   * no RELATIVE/IRELATIVE/COPY/TEXTREL, no DYNAMIC TLS
//   * one PT_TLS segment
// So a manual loader only has to: map PT_LOADs at fixed vaddrs, copy file
// contents, resolve every GOT slot against our shim registry, and hand over.
#include "elf_loader.h"
#include "shim_common.h"
#include "../shim/auto_imports.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <elf.h>
#include <dlfcn.h>
#include <vector>

namespace fo2 {

namespace {

bool map_fixed(uint64_t vaddr, size_t len, int prot) {
    void* p = mmap((void*)(uintptr_t)vaddr, len, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        loge("mmap(0x%llx, %zu) failed", (unsigned long long)vaddr, len);
        return false;
    }
    return true;
}

} // namespace

bool elf_load(const char* path, GuestImage& img) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        loge("cannot open guest binary: %s", path);
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        loge("bad guest binary size: %s", path);
        close(fd);
        return false;
    }

    Elf64_Ehdr ehdr{};
    if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr) ||
        memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_machine != EM_AARCH64 || ehdr.e_type != ET_EXEC) {
        loge("guest binary is not an AArch64 ET_EXEC ELF: %s", path);
        close(fd);
        return false;
    }

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    if (pread(fd, phdrs.data(), sizeof(Elf64_Phdr) * ehdr.e_phnum, ehdr.e_phoff) !=
        (ssize_t)(sizeof(Elf64_Phdr) * ehdr.e_phnum)) {
        loge("failed to read program headers");
        close(fd);
        return false;
    }

    const Elf64_Phdr* tls_ph = nullptr;
    uint64_t min_va = UINT64_MAX, max_va = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD) {
            if (ph.p_vaddr < min_va) min_va = ph.p_vaddr;
            uint64_t end = ph.p_vaddr + ph.p_memsz;
            if (end > max_va) max_va = end;
        } else if (ph.p_type == PT_TLS) {
            tls_ph = &ph;
        }
    }
    if (min_va == UINT64_MAX) {
        loge("guest has no PT_LOAD segments");
        close(fd);
        return false;
    }

    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t span_start = min_va & ~(uint64_t)(page - 1);
    const uint64_t span_end = (max_va + page - 1) & ~(uint64_t)(page - 1);

    // Reserve the whole image span at the fixed address. MAP_FIXED_NOREPLACE
    // gives a clean error if something already lives there.
    if (!map_fixed(span_start, span_end - span_start, PROT_READ | PROT_WRITE)) {
        loge("guest image span 0x%llx-0x%llx is not free on this device",
             (unsigned long long)span_start, (unsigned long long)span_end);
        close(fd);
        return false;
    }

    // Copy PT_LOAD file data into the mapping.
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        uint8_t* dst = (uint8_t*)(uintptr_t)ph.p_vaddr;
        off_t done = 0;
        while (done < (off_t)ph.p_filesz) {
            ssize_t n = pread(fd, dst + done, ph.p_filesz - done, ph.p_offset + done);
            if (n <= 0) {
                loge("short read copying PT_LOAD");
                close(fd);
                return false;
            }
            done += n;
        }
        // memsz > filesz part (.bss) is already zero from the anonymous mapping.
    }

    // Apply the GOT relocations.
    bool ok = true;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn* dyns = (const Elf64_Dyn*)(uintptr_t)ph.p_vaddr;
        const size_t ndyn = ph.p_filesz / sizeof(Elf64_Dyn);
        const Elf64_Rela* rela = nullptr;
        size_t relasz = 0;
        const Elf64_Rela* jmprel = nullptr;
        size_t pltrelsz = 0;
        const Elf64_Sym* symtab = nullptr;
        const char* strtab = nullptr;
        for (size_t i = 0; i < ndyn && dyns[i].d_tag != DT_NULL; ++i) {
            const Elf64_Dyn& d = dyns[i];
            switch (d.d_tag) {
                case DT_RELA:    rela = (const Elf64_Rela*)(uintptr_t)d.d_un.d_ptr; break;
                case DT_RELASZ:  relasz = d.d_un.d_val; break;
                case DT_JMPREL:  jmprel = (const Elf64_Rela*)(uintptr_t)d.d_un.d_ptr; break;
                case DT_PLTRELSZ: pltrelsz = d.d_un.d_val; break;
                case DT_SYMTAB:  symtab = (const Elf64_Sym*)(uintptr_t)d.d_un.d_ptr; break;
                case DT_STRTAB:  strtab = (const char*)(uintptr_t)d.d_un.d_ptr; break;
                case DT_INIT_ARRAY:    img.init_array_ptr = (void**)(uintptr_t)d.d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ:  img.init_array_count = d.d_un.d_val / sizeof(void*); break;
                case DT_FINI_ARRAY:    img.fini_array_ptr = (void**)(uintptr_t)d.d_un.d_ptr; break;
                case DT_FINI_ARRAYSZ:  img.fini_array_count = d.d_un.d_val / sizeof(void*); break;
                default: break;
            }
        }
        auto apply = [&](const Elf64_Rela* r, size_t count) {
            for (size_t i = 0; i < count; ++i, ++r) {
                if (r->r_addend != 0) {
                    loge("unexpected reloc addend at slot %zu", i);
                    ok = false;
                    return;
                }
                uint32_t type = ELF64_R_TYPE(r->r_info);
                if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) {
                    loge("unsupported reloc type %u", type);
                    ok = false;
                    return;
                }
                const char* name = strtab + symtab[ELF64_R_SYM(r->r_info)].st_name;
                void* addr = find_guest_symbol(name);
                if (addr == nullptr && find_import(name) &&
                    find_import(name)->kind != SymKind::Weak0) {
                    loge("cannot resolve guest import: %s", name);
                    ok = false;
                    return;
                }
                *(void**)(uintptr_t)r->r_offset = addr;
            }
        };
        if (rela) apply(rela, relasz / sizeof(Elf64_Rela));
        if (jmprel) apply(jmprel, pltrelsz / sizeof(Elf64_Rela));
        break;
    }

    // Seal executable segments: reprotect PF_X as R+X (pages were mapped RW for
    // relocation writing; GOT/data segments stay RW).
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD) continue;
        int prot = 0;
        if (ph.p_flags & PF_R) prot |= PROT_READ;
        if (ph.p_flags & PF_W) prot |= PROT_WRITE;
        if (ph.p_flags & PF_X) prot |= PROT_EXEC;
        uint64_t s = ph.p_vaddr & ~(uint64_t)(page - 1);
        uint64_t e = (ph.p_vaddr + ph.p_memsz + page - 1) & ~(uint64_t)(page - 1);
        if (mprotect((void*)(uintptr_t)s, e - s, prot) != 0) {
            loge("mprotect failed for segment 0x%llx", (unsigned long long)s);
            ok = false;
        }
    }

    // TLS initial image.
    if (tls_ph && tls_ph->p_memsz > 0) {
        img.tls_memsz = tls_ph->p_memsz;
        img.tls_filesz = tls_ph->p_filesz;
        img.tls_align = tls_ph->p_align ? tls_ph->p_align : 1;
        img.tls_image = (uint8_t*)calloc(1, img.tls_memsz);
        if (!img.tls_image) {
            loge("out of memory for TLS image");
            ok = false;
        } else if (img.tls_filesz > 0) {
            off_t done = 0;
            while (done < (off_t)img.tls_filesz) {
                ssize_t n = pread(fd, img.tls_image + done, img.tls_filesz - done,
                                  tls_ph->p_offset + done);
                if (n <= 0) { loge("short read copying TLS image"); ok = false; break; }
                done += n;
            }
        }
    }

    img.entry = ehdr.e_entry;
    img.base_vaddr = min_va;
    img.end_vaddr = max_va;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_PHDR) { img.phdr_vaddr = ph.p_vaddr; img.phnum = ph.p_filesz / sizeof(Elf64_Phdr); }
    }
    if (!img.phdr_vaddr) {  // fall back: phdrs live inside the first PT_LOAD file image
        img.phdr_vaddr = min_va + ehdr.e_phoff;
        img.phnum = ehdr.e_phnum;
    }

    close(fd);
    if (ok) {
        logi("guest image mapped: 0x%llx-0x%llx entry 0x%llx tls %zu",
             (unsigned long long)img.base_vaddr, (unsigned long long)img.end_vaddr,
             (unsigned long long)img.entry, img.tls_memsz);
    }
    return ok;
}

} // namespace fo2
