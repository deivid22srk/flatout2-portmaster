// core/elf_loader.cpp — in-process loader for the recompiled FlatOut 2 guest.
//
// fo2.aarch64 is an ET_EXEC (non-PIE) AArch64 glibc binary at a fixed base
// (0x1000000) with BIND_NOW. Its dynamic surface, verified with readelf:
//   * 370 JUMP_SLOT + 8 GLOB_DAT relocations — the only reloc kinds present
//   * no RELATIVE/IRELATIVE/COPY/TEXTREL, no DYNAMIC TLS
//   * one PT_TLS segment
// So a manual loader only has to: map PT_LOADs at fixed vaddrs, copy file
// contents, resolve every GOT slot against our shim registry, and hand over.
//
// The link-time base is NOT guaranteed free on real devices: zygote-inherited
// reservations (ART heap spaces, JIT code cache, linker gap mappings, vendor
// blobs) differ per device and can cover parts of the low 4 GiB. Loading
// therefore tries, in order:
//   1. the link-time address (MAP_FIXED_NOREPLACE);
//   2. the link-time address after dropping overlapping *pure gap* mappings
//      (anonymous, unnamed, PROT_NONE — unused reservations, safe to unmap);
//   3. the lowest free 64 KiB-aligned hole below 2 GiB, then below 4 GiB.
//      The image is slid by a delta and every 8-byte word of the file image
//      that points into the old span is rebased: that covers .dynamic d_ptr
//      values, the RELA r_offsets, init/fini arrays, phdr/ELF-header vaddrs,
//      .eh_frame absolute FDE pointers and all data function-pointer tables.
//      Verified against the actual binary (see scripts/analyze_fo2elf.py):
//      no absolute 32-bit jump tables (AArch64/clang emits PC-relative ones),
//      so an 8-byte rebase pass is sound. glibc's __libc_setup_tls and the
//      unwinder read the rebased phdrs/.eh_frame and keep working.
//   4. a kernel-chosen address as the last resort (logged loudly).
// Every failed attempt dumps the overlapping /proc/self/maps entries so a
// device-specific log pinpoints the culprit immediately.
#include "elf_loader.h"
#include "shim_common.h"
#include "../shim/auto_imports.h"

#include <cerrno>
#include <cinttypes>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <elf.h>
#include <dlfcn.h>
#include <algorithm>
#include <string>
#include <vector>

namespace fo2 {

namespace {
bool map_at(uint64_t vaddr, size_t len, int prot) {
    void* p = mmap((void*)(uintptr_t)vaddr, len, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        loge("mmap(0x%" PRIx64 ", %zu) failed: %s (errno %d)",
             vaddr, len, strerror(errno), errno);
        return false;
    }
    return true;
}

// Lowest 64 KiB-aligned hole of at least `span` bytes inside [lo, hi), from
// the snapshot in `maps`. Returns 0 when nothing fits.
uint64_t find_free_hole(const std::vector<SelfMapsEntry>& maps, uint64_t span,
                        uint64_t lo, uint64_t hi) {
    if (hi <= lo || hi - lo < span) return 0;
    std::vector<std::pair<uint64_t, uint64_t>> used;
    for (const auto& m : maps) {
        uint64_t s = std::max(m.start, lo), e = std::min(m.end, hi);
        if (s < e) used.emplace_back(s, e);
    }
    std::sort(used.begin(), used.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto& r : used) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    constexpr uint64_t kAlign = 0x10000;  // p_align of the guest segments
    uint64_t prev = lo;
    for (const auto& r : merged) {
        uint64_t cand = (prev + kAlign - 1) & ~(kAlign - 1);
        if (r.first > cand && r.first - cand >= span) return cand;
        prev = std::max(prev, r.second);
    }
    uint64_t cand = (prev + kAlign - 1) & ~(kAlign - 1);
    if (cand <= hi - span) return cand;
    return 0;
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
    const uint64_t span_len = ((max_va + page - 1) & ~(uint64_t)(page - 1)) - span_start;

    // ---- acquire the address range ----------------------------------------
    // delta is how far every guest vaddr moved from its link-time value.
    uint64_t delta = 0;
    if (map_at(span_start, span_len, PROT_READ | PROT_WRITE)) {
        logi("guest image at link-time base 0x%" PRIx64, span_start);
    } else {
        loge("link-time span 0x%" PRIx64 "-0x%" PRIx64 " is taken; diagnosing",
             span_start, span_start + span_len);
        log_overlapping_maps(span_start, span_start + span_len, "guest image");

        // 2nd try: drop pure PROT_NONE gap reservations and retry in place.
        std::vector<SelfMapsEntry> maps;
        bool reclaimed = false;
        if (read_self_maps(&maps)) {
            bool all_gaps = true;
            std::vector<std::pair<uint64_t, uint64_t>> drops;
            for (const auto& m : maps) {
                if (m.start < span_start + span_len && span_start < m.end) {
                    bool gap = m.perms == "---p" && m.inode == 0 && m.path.empty();
                    if (gap) {
                        drops.emplace_back(std::max(m.start, span_start),
                                           std::min(m.end, span_start + span_len));
                    } else {
                        all_gaps = false;
                        break;
                    }
                }
            }
            if (all_gaps && !drops.empty()) {
                for (const auto& d : drops) {
                    if (munmap((void*)(uintptr_t)d.first, d.second - d.first) == 0) {
                        logi("dropped PROT_NONE gap 0x%" PRIx64 "-0x%" PRIx64,
                             d.first, d.second);
                        reclaimed = true;
                    }
                }
            }
        }
        if (reclaimed && map_at(span_start, span_len, PROT_READ | PROT_WRITE)) {
            logi("guest image at link-time base after gap reclaim 0x%" PRIx64, span_start);
        } else {
            // 3rd try: slide the image into the lowest free hole we can find.
            std::vector<SelfMapsEntry> fresh;
            uint64_t cand = 0;
            if (read_self_maps(&fresh)) {
                cand = find_free_hole(fresh, span_len, 0x1000000, 0x80000000ull);
                if (cand == 0) cand = find_free_hole(fresh, span_len, 0x80000000ull, 0x100000000ull);
            }
            bool mapped = false;
            if (cand != 0) {
                mapped = map_at(cand, span_len, PROT_READ | PROT_WRITE);
            }
            if (!mapped) {
                // 4th try: let the kernel pick (any address, logged loudly).
                void* p = mmap(nullptr, span_len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) {
                    loge("guest image mapping failed everywhere (%zu bytes)", span_len);
                    close(fd);
                    return false;
                }
                cand = (uint64_t)(uintptr_t)p;
                loge("last resort: guest image at kernel-chosen 0x%" PRIx64, cand);
            } else {
                logi("guest image slid to 0x%" PRIx64, cand);
            }
            delta = cand - span_start;
        }
    }

    // Copy PT_LOAD file data into the mapping (slid).
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        uint8_t* dst = (uint8_t*)(uintptr_t)(ph.p_vaddr + delta);
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

    // Rebase pass: when the image slid, every 8-byte word of the file image
    // that holds an absolute value inside the old span is an internal pointer
    // (dynamic entries, RELA r_offsets, init/fini arrays, phdr vaddrs, FDE
    // addresses, function tables). Move them along. Values below the span are
    // x86 guest addresses / sizes and stay untouched; nothing in this binary
    // uses absolute 32-bit code pointers (clang emits PC-relative jump tables).
    size_t fixed = 0;
    if (delta != 0) {
        for (const auto& ph : phdrs) {
            if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
            uint64_t* w = (uint64_t*)(uintptr_t)(ph.p_vaddr + delta);
            const size_t words = ph.p_filesz / sizeof(uint64_t);
            for (size_t i = 0; i < words; ++i) {
                if (w[i] >= min_va && w[i] < max_va) {
                    w[i] += delta;
                    ++fixed;
                }
            }
        }
        logi("rebased %" PRIu64 " internal pointers by +0x%" PRIx64 " (image 0x%" PRIx64
             " -> 0x%" PRIx64 ")", (uint64_t)fixed, delta, min_va, min_va + delta);
    }

    // Apply the GOT relocations. The dynamic table was read from the mapped
    // image, so with a slide its d_ptr values (and the RELA r_offsets) are
    // already rebased by the pass above.
    bool ok = true;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn* dyns = (const Elf64_Dyn*)(uintptr_t)(ph.p_vaddr + delta);
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
        uint64_t s = (ph.p_vaddr + delta) & ~(uint64_t)(page - 1);
        uint64_t e = (ph.p_vaddr + delta + ph.p_memsz + page - 1) & ~(uint64_t)(page - 1);
        if (mprotect((void*)(uintptr_t)s, e - s, prot) != 0) {
            loge("mprotect failed for segment 0x%" PRIx64, s);
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

    img.entry = ehdr.e_entry + delta;
    img.base_vaddr = min_va + delta;
    img.end_vaddr = max_va + delta;
    img.slide_delta = delta;
    img.phdrs.assign(phdrs.begin(), phdrs.end());  // link-time values (NOT rebased)
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_PHDR) { img.phdr_vaddr = ph.p_vaddr + delta; img.phnum = ph.p_filesz / sizeof(Elf64_Phdr); }
    }
    if (!img.phdr_vaddr) {  // fall back: phdrs live inside the first PT_LOAD file image
        img.phdr_vaddr = min_va + delta + ehdr.e_phoff;
        img.phnum = ehdr.e_phnum;
    }

    close(fd);
    if (ok) {
        logi("guest image mapped: 0x%" PRIx64 "-0x%" PRIx64 " entry 0x%" PRIx64
             " tls %zu (link base 0x%" PRIx64 ", delta 0x%" PRIx64 ")",
             img.base_vaddr, img.end_vaddr, img.entry, img.tls_memsz, min_va, delta);
    }
    return ok;
}

} // namespace fo2
