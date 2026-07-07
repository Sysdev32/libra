#include <stdint.h>
#include <stddef.h>
#include <drivers/elf.h>
#include <drivers/vfs.h>
#include <drivers/alloc.h> // Using your native kmalloc/kfree

#ifndef DT_HASH
#define DT_HASH 4
#endif

// ============================================================================
// Global Library Tracking (Replaces the TODOs)
// ============================================================================

typedef struct loaded_module {
    char name[256];
    Elf64_Sym *symtab;
    const char *strtab;
    uint32_t sym_count;
    uint64_t load_bias;
    struct loaded_module *next;
} loaded_module_t;

static loaded_module_t *global_module_list = NULL;

// ============================================================================
// Internal utilities
// ============================================================================

static int elf_strcmp(const char *a, const char *b) {
    if (!a || !b) return -1;
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

static size_t elf_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void elf_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

uint64_t elf_needed_mem(void *raw_elf_data) {
    ElfLoadResult probe = load_elf(raw_elf_data, 0, 0);
    return probe.mem_size;
}

uint64_t elf_vaddr(void *raw_elf_data) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F')
        return 0;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)raw_elf_data + ehdr->e_phoff);
    uint64_t lowest = UINT64_MAX;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < lowest) {
            lowest = phdrs[i].p_vaddr;
        }
    }
    return lowest == UINT64_MAX ? 0 : lowest;
}

static inline void* vma_to_hhdm(uint64_t vma, uint64_t vma_base, uint64_t phys_base) {
    return (void*)(phys_base + (vma - vma_base) + HHDM_OFFSET);
}

// ============================================================================
// Symbol resolution & Library Loading
// ============================================================================

// fully implemented cross-library symbol resolution
static uint64_t resolve_external_symbol(const char *name) {
    loaded_module_t *curr = global_module_list;
    
    while (curr) {
        if (curr->symtab && curr->strtab) {
            // Start at 1, index 0 is always the undefined symbol
            for (uint32_t i = 1; i < curr->sym_count; i++) {
                Elf64_Sym *s = &curr->symtab[i];
                if (s->st_name && s->st_shndx) {
                    if (elf_strcmp(curr->strtab + s->st_name, name) == 0) {
                        return s->st_value + curr->load_bias;
                    }
                }
            }
        }
        curr = curr->next;
    }
    return 0; 
}

// Uses your VFS to load a required shared object
static void load_shared_library(const char *lib_name) {
    // 1. Prevent infinite recursion by checking if it's already loaded
    loaded_module_t *curr = global_module_list;
    while (curr) {
        if (elf_strcmp(curr->name, lib_name) == 0) return;
        curr = curr->next;
    }

    // 2. Build the path (assuming libs are in /lib/)
    char path[256];
    elf_strcpy(path, "/lib/");
    
    size_t prefix_len = 5; 
    size_t name_len = elf_strlen(lib_name);
    if (prefix_len + name_len < 256) {
        elf_strcpy(path + prefix_len, lib_name);
    } else {
        return; // Path too long
    }

    // 3. Open file via VFS
    int fd = vfs_open(path);
    if (fd < 0) return; // Library not found

    // 4. Stat the file to get its size
    struct vfs_stat st;
    if (vfs_fstat(fd, &st) < 0) {
        vfs_free_fd(fd);
        return;
    }

    // 5. Allocate a staging buffer and read the raw ELF data
    void *file_buf = kmalloc(st.st_size);
    if (!file_buf) {
        vfs_free_fd(fd);
        return;
    }

    vfs_read(fd, file_buf, st.st_size, 0);
    vfs_free_fd(fd);

    // 6. Measure how much memory this library needs mapped
    uint64_t needed_mem = elf_needed_mem(file_buf);
    if (needed_mem == 0) {
        kfree(file_buf);
        return;
    }

    // 7. Allocate memory for the library to live in using kmalloc
    void *lib_memory = kmalloc(needed_mem);
    if (!lib_memory) {
        kfree(file_buf);
        return;
    }

    uint64_t load_vma = (uint64_t)lib_memory;
    uint64_t phys_base = load_vma - HHDM_OFFSET;

    // 8. Recursively load the library
    load_elf(file_buf, phys_base, load_vma);

    // 9. Register the library in the global list for symbol resolution
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)lib_memory;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)lib_memory + ehdr->e_phoff);
    
    Elf64_Dyn *dynamic_table = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            // Since it's already loaded, we just point directly to it
            dynamic_table = (Elf64_Dyn *)((uint8_t *)lib_memory + phdrs[i].p_vaddr);
            break;
        }
    }

    if (dynamic_table) {
        Elf64_Sym *symtab = NULL;
        const char *strtab = NULL;
        uint32_t *hashtab = NULL;

        for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_SYMTAB) symtab = (Elf64_Sym *)(load_vma + d->d_un.d_ptr);
            if (d->d_tag == DT_STRTAB) strtab = (const char *)(load_vma + d->d_un.d_ptr);
            if (d->d_tag == DT_HASH)   hashtab = (uint32_t *)(load_vma + d->d_un.d_ptr);
        }

        if (symtab && strtab) {
            loaded_module_t *mod = kmalloc(sizeof(loaded_module_t));
            if (mod) {
                elf_strcpy(mod->name, lib_name);
                mod->symtab = symtab;
                mod->strtab = strtab;
                mod->load_bias = load_vma;
                // nchain is at index 1 of the hash table, it dictates the symbol count
                mod->sym_count = hashtab ? hashtab[1] : 8192; 
                mod->next = global_module_list;
                global_module_list = mod;
            }
        }
    }

    // 10. Cleanup staging buffer
    kfree(file_buf);
}

// ============================================================================
// load_elf
// ============================================================================

ElfLoadResult load_elf(void *raw_elf_data, uint64_t physical_base, uint64_t load_vma) {
    ElfLoadResult result = {0, 0, 0};
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return result;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return result;
    }

    int is_pic = (ehdr->e_type == ET_DYN);
    uint64_t load_bias = is_pic ? load_vma : 0;
    int probe_only = (physical_base == 0);

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return result;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)raw_elf_data + ehdr->e_phoff);

    uint64_t exec_base = 0;
    if (!is_pic) {
        exec_base = (uint64_t)-1;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < exec_base)
                exec_base = phdrs[i].p_vaddr;
        }
        if (exec_base == (uint64_t)-1) exec_base = load_vma;
    }

    uint64_t vma_base = is_pic ? 0 : exec_base;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t phys_offset = ph->p_vaddr - vma_base;
        uint64_t segment_end = phys_offset + ph->p_memsz;
        if (segment_end > result.mem_size) result.mem_size = segment_end;

        if (probe_only) continue;

        uint8_t *dest = (uint8_t *)vma_to_hhdm(ph->p_vaddr, vma_base, physical_base);
        uint8_t *src  = (uint8_t *)raw_elf_data + ph->p_offset;

        if (ph->p_filesz > 0) elf_memcpy(dest, src, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) elf_memset(dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    result.entry_point  = ehdr->e_entry + load_bias;
    result.virtual_addr = load_vma;

    if (probe_only) return result;

    Elf64_Dyn *dynamic_table = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynamic_table = (Elf64_Dyn *)vma_to_hhdm(phdrs[i].p_vaddr, vma_base, physical_base);
            break;
        }
    }

    if (!dynamic_table) return result; 

    Elf64_Sym  *symtab = NULL;
    const char *strtab = NULL;
    Elf64_Rela *relas  = NULL;
    uint64_t   rela_sz = 0;
    uint64_t   rela_ent = 0;

    for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = (Elf64_Sym *)vma_to_hhdm(d->d_un.d_ptr, vma_base, physical_base); break;
            case DT_STRTAB: strtab = (const char *)vma_to_hhdm(d->d_un.d_ptr, vma_base, physical_base); break;
            case DT_RELA:   relas  = (Elf64_Rela *)vma_to_hhdm(d->d_un.d_ptr, vma_base, physical_base); break;
            case DT_RELASZ: rela_sz = d->d_un.d_val; break;
            case DT_RELAENT:rela_ent = d->d_un.d_val; break;
        }
    }

    for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED && strtab) {
            const char *lib_name = strtab + d->d_un.d_val;
            load_shared_library(lib_name);
        }
    }

    if (relas && rela_sz > 0 && rela_ent > 0) {
        uint64_t n_relas = rela_sz / rela_ent;

        for (uint64_t j = 0; j < n_relas; j++) {
            Elf64_Rela *rel = &relas[j];
            uint64_t type    = ELF64_R_TYPE(rel->r_info);
            uint64_t sym_idx = ELF64_R_SYM(rel->r_info);

            uint64_t *patch = (uint64_t *)vma_to_hhdm(rel->r_offset, vma_base, physical_base);
            uint64_t sym_val = 0;

            if (sym_idx && symtab && strtab) {
                Elf64_Sym *sym = &symtab[sym_idx];
                const char *sym_name = strtab + sym->st_name;

                if (sym->st_shndx == 0) {
                    sym_val = resolve_external_symbol(sym_name);
                } else {
                    sym_val = sym->st_value + load_bias;
                }
            }

            switch (type) {
                case R_X86_64_RELATIVE:
                    *patch = load_bias + (uint64_t)rel->r_addend;
                    break;
                case R_X86_64_64:
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    *patch = sym_val + (uint64_t)rel->r_addend;
                    break;
            }
        }
    }

    return result;
}