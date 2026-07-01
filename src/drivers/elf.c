#include <stdint.h>
#include <stddef.h>
#include <drivers/elf.h>
// ============================================================================
// Internal utilities
// ============================================================================

static int elf_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void elf_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static void elf_memset(void *dst, int v, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)v;
}
uint64_t elf_vaddr(void *raw_elf_data) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
        return 0;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
        return 0;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)raw_elf_data + ehdr->e_phoff);

    uint64_t lowest = UINT64_MAX;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_vaddr < lowest)
            lowest = ph->p_vaddr;
    }

    return lowest == UINT64_MAX ? 0 : lowest;
}
// ============================================================================
// Symbol resolver — used to fix up undefined symbols against internal exports
// ============================================================================

static uint64_t resolve_symbol(Elf64_Sym *symtab, uint64_t sym_count,
                                const char *strtab, const char *name,
                                uint64_t load_bias) {
    if (!symtab || !strtab) return 0;
    for (uint64_t i = 0; i < sym_count; i++) {
        Elf64_Sym *s = &symtab[i];
        if (s->st_name && s->st_shndx)
            if (elf_strcmp(strtab + s->st_name, name) == 0)
                return s->st_value + load_bias;
    }
    return 0;
}

// ============================================================================
// load_elf
//
//   raw_elf_data      — HHDM pointer to the raw ELF bytes (staging buffer)
//   physical_base     — physical address of the destination mapping;
//                       pass 0 for a probe-only call (no writes, returns mem_size)
//   load_vma          — the virtual address this binary will run at
//                       for ET_DYN this becomes the load_bias
//                       for ET_EXEC this must match the binary's expected base
// ============================================================================

ElfLoadResult load_elf(void *raw_elf_data, uint64_t physical_base, uint64_t load_vma) {
    ElfLoadResult result = {0, 0, 0};

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    // -------------------------------------------------------------------------
    // Phase 1: Validate ELF magic + supported type
    // -------------------------------------------------------------------------
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return result;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return result;
    }

    int is_pic = (ehdr->e_type == ET_DYN);

    // For PIE/PIC: load_vma is the bias added to every VMA in the binary.
    // For ET_EXEC: load_vma is just the expected fixed base (used for offset math).
    uint64_t load_bias = is_pic ? load_vma : 0;

    // probe_only: caller passed physical_base == 0 meaning "just measure, don't write"
    int probe_only = (physical_base == 0);

    // -------------------------------------------------------------------------
    // Phase 2: Walk PT_LOAD segments
    //   - Compute mem_size (always)
    //   - Copy/zero segments into physical memory (unless probe_only)
    // -------------------------------------------------------------------------
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return result;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)raw_elf_data + ehdr->e_phoff);

    // For PIE the p_vaddr fields are offsets from 0.
    // For ET_EXEC they are absolute VMAs.
    // In both cases the physical offset from physical_base is:
    //   phys_offset = p_vaddr - (is_pic ? 0 : exec_base)
    // which simplifies to:
    //   phys_offset = p_vaddr                          (PIE, already relative)
    //   phys_offset = p_vaddr - expected_exec_base     (ET_EXEC)
    //
    // We also need the lowest PT_LOAD vaddr to handle ET_EXEC binaries that
    // don't start their first segment exactly at load_vma.
    uint64_t exec_base = 0;
    if (!is_pic) {
        // Find the lowest PT_LOAD vaddr to use as the physical base anchor
        exec_base = (uint64_t)-1;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD)
                if (phdrs[i].p_vaddr < exec_base)
                    exec_base = phdrs[i].p_vaddr;
        }
        if (exec_base == (uint64_t)-1) exec_base = load_vma;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        // Physical byte offset from physical_base where this segment lands
        uint64_t phys_offset = is_pic ? ph->p_vaddr
                                      : (ph->p_vaddr - exec_base);

        // Track the furthest byte needed
        uint64_t segment_end = phys_offset + ph->p_memsz;
        if (segment_end > result.mem_size)
            result.mem_size = segment_end;

        if (probe_only) continue;

        uint8_t *dest = (uint8_t *)(physical_base + phys_offset + HHDM_OFFSET);
        uint8_t *src  = (uint8_t *)raw_elf_data + ph->p_offset;

        if (ph->p_filesz > 0)
            elf_memcpy(dest, src, ph->p_filesz);

        // Zero .bss tail
        if (ph->p_memsz > ph->p_filesz)
            elf_memset(dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    // Entry point is always an absolute VMA after applying bias
    result.entry_point  = ehdr->e_entry + load_bias;
    result.virtual_addr = load_vma;

    if (probe_only) return result;

    // -------------------------------------------------------------------------
    // Phase 3: Locate symbol + string tables for relocation resolution
    // -------------------------------------------------------------------------
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return result;

    Elf64_Shdr *shdrs   = (Elf64_Shdr *)((uint8_t *)raw_elf_data + ehdr->e_shoff);
    Elf64_Sym  *symtab  = NULL;
    const char *strtab  = NULL;
    uint64_t    sym_count = 0;

    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if ((sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM)
                && sh->sh_entsize > 0) {
            symtab    = (Elf64_Sym  *)((uint8_t *)raw_elf_data + sh->sh_offset);
            sym_count = sh->sh_size / sh->sh_entsize;
            strtab    = (const char *)((uint8_t *)raw_elf_data + shdrs[sh->sh_link].sh_offset);
            break;
        }
    }

    // -------------------------------------------------------------------------
    // Phase 4: Apply relocations
    //
    // For both PIE and ET_EXEC:
    //   rel->r_offset is a VMA.
    //   Physical patch address = physical_base + (r_offset - vma_base) + HHDM_OFFSET
    //   where vma_base = load_bias for PIE (r_offset relative to 0)
    //                  = exec_base  for ET_EXEC
    // -------------------------------------------------------------------------
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        Elf64_Shdr *sh = &shdrs[i];
        if (sh->sh_type != SHT_RELA || sh->sh_entsize == 0) continue;

        Elf64_Rela *relas   = (Elf64_Rela *)((uint8_t *)raw_elf_data + sh->sh_offset);
        uint64_t    n_relas = sh->sh_size / sh->sh_entsize;

        for (uint64_t j = 0; j < n_relas; j++) {
            Elf64_Rela *rel     = &relas[j];
            uint64_t    type    = ELF64_R_TYPE(rel->r_info);
            uint64_t    sym_idx = ELF64_R_SYM(rel->r_info);

            // r_offset is a VMA; convert to a physical offset from physical_base
            uint64_t vma_base    = is_pic ? 0 : exec_base;
            uint64_t phys_offset = rel->r_offset - vma_base;
            uint64_t *patch      = (uint64_t *)(physical_base + phys_offset + HHDM_OFFSET);

            // Resolve the symbol if this relocation references one
            uint64_t sym_val = 0;
            if (sym_idx && symtab && strtab) {
                Elf64_Sym *sym = &symtab[sym_idx];
                if (sym->st_shndx == 0) {
                    // External/undefined: try to find it among internal symbols
                    sym_val = resolve_symbol(symtab, sym_count, strtab,
                                             strtab + sym->st_name, load_bias);
                } else {
                    // Defined symbol: value is relative to binary base for PIE,
                    // absolute for ET_EXEC
                    sym_val = sym->st_value + load_bias;
                }
            }

            switch (type) {
                case R_X86_64_RELATIVE:
                    // S = load_bias + addend (no symbol involved)
                    *patch = load_bias + (uint64_t)rel->r_addend;
                    break;

                case R_X86_64_64:
                    // S + A
                    *patch = sym_val + (uint64_t)rel->r_addend;
                    break;

                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    // S  (addend is always 0 for these but apply it anyway)
                    *patch = sym_val + (uint64_t)rel->r_addend;
                    break;

                default:
                    // Unknown relocation type — silently skip
                    break;
            }
        }
    }

    return result;
}