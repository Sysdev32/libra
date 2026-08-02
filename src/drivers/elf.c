#include <stdint.h>
#include <stddef.h>
#include <drivers/elf.h>
#include <fs/mnt.h>
#include <drivers/fb.h>

typedef uint64_t page_table_t;

// Standard x86_64 Paging Definitions
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096ULL
#endif

#ifndef PAGE_MASK
#define PAGE_MASK ~(PAGE_SIZE - 1ULL)
#endif

// Kernel High-Half Direct Mapping Base Offset (Standard x86_64 Higher Half Kernel)
#ifndef KERNEL_VIRTUAL_BASE
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL
#endif

// Convert Kernel Virtual Address to Physical Address without PMM
#define VIRT_TO_PHYS(virt) ((uint64_t)(virt) - KERNEL_VIRTUAL_BASE)

// Page Table Flags
#ifndef VMM_PRESENT
#define VMM_PRESENT  (1ULL << 0)
#endif
#ifndef VMM_WRITABLE
#define VMM_WRITABLE (1ULL << 1)
#endif
#ifndef VMM_USER
#define VMM_USER     (1ULL << 2)
#endif

// External VMM declarations
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern page_table_t *vmm_get_current_pml4(void);

// ============================================================================
// 1. Dynamic ELF Specifications & Architectural Relocation Definitions
// ============================================================================

#ifndef DT_NULL
#define DT_NULL 0
#endif
#ifndef DT_NEEDED
#define DT_NEEDED 1
#endif
#ifndef DT_PLTRELSZ
#define DT_PLTRELSZ 2
#endif
#ifndef DT_HASH
#define DT_HASH 4
#endif
#ifndef DT_STRTAB
#define DT_STRTAB 5
#endif
#ifndef DT_SYMTAB
#define DT_SYMTAB 6
#endif
#ifndef DT_RELA
#define DT_RELA 7
#endif
#ifndef DT_RELASZ
#define DT_RELASZ 8
#endif
#ifndef DT_RELAENT
#define DT_RELAENT 9
#endif
#ifndef DT_STRSZ
#define DT_STRSZ 10
#endif
#ifndef DT_SYMENT
#define DT_SYMENT 11
#endif
#ifndef DT_JMPREL
#define DT_JMPREL 23
#endif

#ifndef R_X86_64_NONE
#define R_X86_64_NONE      0
#endif
#ifndef R_X86_64_64
#define R_X86_64_64        1
#endif
#ifndef R_X86_64_PC32
#define R_X86_64_PC32      2
#endif
#ifndef R_X86_64_GOT32
#define R_X86_64_GOT32     3
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32     4
#endif
#ifndef R_X86_64_COPY
#define R_X86_64_COPY      5
#endif
#ifndef R_X86_64_GLOB_DAT
#define R_X86_64_GLOB_DAT  6
#endif
#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT 7
#endif
#ifndef R_X86_64_RELATIVE
#define R_X86_64_RELATIVE  8
#endif
#ifndef R_X86_64_32
#define R_X86_64_32        10
#endif
#ifndef R_X86_64_32S
#define R_X86_64_32S       11
#endif

// ============================================================================
// 2. Data Structures & Static Memory Pools
// ============================================================================

typedef enum module_state {
    MODULE_STATE_UNLOADED = 0,
    MODULE_STATE_REGISTERED,
    MODULE_STATE_LOADED,
    MODULE_STATE_RELOCATED
} module_state_t;

typedef struct loaded_module {
    char name[256];
    Elf64_Sym *symtab;
    const char *strtab;
    uint32_t sym_count;
    uint64_t load_bias;
    uint64_t vma_base;
    void *load_vma;
    size_t allocated_size;
    int is_pic;
    Elf64_Dyn *dynamic_table;
    module_state_t state;
    struct loaded_module *next;
} loaded_module_t;

static loaded_module_t *global_module_list = NULL;

// Internal loader metadata pool
#define ELF_INTERNAL_POOL_SIZE (1024 * 1024 * 16)
static uint8_t elf_internal_pool[ELF_INTERNAL_POOL_SIZE];
static size_t  elf_pool_offset = 0;

// 32MB Static Physical Backstore Pool
#define LIB_PHYS_POOL_SIZE (1024 * 1024 * 32)
static uint8_t __attribute__((aligned(4096))) lib_phys_pool[LIB_PHYS_POOL_SIZE];
static size_t  lib_phys_pool_offset = 0;

// Lower-Half Virtual Address Bump Pointer for Libraries
static uintptr_t user_lib_vma_bump = 0x700000000000ULL;

uint64_t elf_needed_mem(void *raw_elf_data);
static loaded_module_t* load_shared_library(const char *lib_name);

// ============================================================================
// 3. Helper Functions
// ============================================================================

static void* static_elf_alloc(size_t size) {
    size_t aligned_size = (size + 7) & ~((size_t)7);

    if (elf_pool_offset + aligned_size > ELF_INTERNAL_POOL_SIZE) {
        printk(LOG_ERROR, "[ELF METADATA ALLOC FATAL] Loader pool exhausted! Offset: 0x%zx + Request: 0x%zx > Limit: 0x%x\n",
               elf_pool_offset, aligned_size, ELF_INTERNAL_POOL_SIZE);
        return NULL;
    }

    void *ptr = &elf_internal_pool[elf_pool_offset];
    elf_pool_offset += aligned_size;
    return ptr;
}

static void* static_phys_pool_alloc(size_t size) {
    size_t aligned_size = (size + PAGE_SIZE - 1ULL) & PAGE_MASK;

    if (lib_phys_pool_offset + aligned_size > LIB_PHYS_POOL_SIZE) {
        printk(LOG_ERROR, "[PHYS POOL FATAL] Static physical backstore exhausted! Offset: 0x%zx + Request: 0x%zx > Limit: 0x%x\n",
               lib_phys_pool_offset, aligned_size, LIB_PHYS_POOL_SIZE);
        return NULL;
    }

    void *ptr = &lib_phys_pool[lib_phys_pool_offset];
    lib_phys_pool_offset += aligned_size;
    return ptr;
}

static inline uintptr_t canonicalize(uintptr_t addr) {
    if (addr & (1ULL << 47)) {
        return addr | 0xFFFF000000000000ULL;
    } else {
        return addr & 0x0000FFFFFFFFFFFFULL;
    }
}

static inline void* elf_vaddr_to_ptr(uint64_t target_vaddr, uint64_t vma_base, void *allocated_vma, int is_pic) {
    if (!allocated_vma) return NULL;

    uintptr_t base_ptr = (uintptr_t)allocated_vma;
    uintptr_t result_addr = 0;

    if (is_pic) {
        if (vma_base > 0 && target_vaddr >= vma_base) {
            result_addr = base_ptr + (target_vaddr - vma_base);
        } else {
            result_addr = base_ptr + target_vaddr;
        }
    } else {
        if (target_vaddr >= vma_base) {
            result_addr = base_ptr + (target_vaddr - vma_base);
        } else {
            printk(LOG_ERROR, "[ELF MAPPER ERROR] Target VADDR 0x%lx precedes base VMA 0x%lx!\n", target_vaddr, vma_base);
            return NULL;
        }
    }

    return (void*)canonicalize(result_addr);
}

static inline int is_valid_write_ptr(void *ptr, void *vma_start, size_t vma_size) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)vma_start;
    uintptr_t end = start + vma_size;

    return (p >= start && (p + sizeof(uint64_t)) <= end);
}

static int elf_strcmp(const char *a, const char *b) {
    if (!a || !b) return -1;
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void elf_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static void elf_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)v;
}

static void elf_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

// Map kernel-backed memory into user page tables
static void map_user_vma_region(page_table_t *pml4, uint64_t virt_start, void* phys_pool_ptr, size_t size) {
    uint64_t start_aligned = virt_start & PAGE_MASK;
    uint64_t end_aligned   = (virt_start + size + PAGE_SIZE - 1ULL) & PAGE_MASK;
    uint64_t curr_phys     = VIRT_TO_PHYS(phys_pool_ptr);
    uint64_t flags         = VMM_PRESENT | VMM_WRITABLE | VMM_USER;

    for (uint64_t vaddr = start_aligned; vaddr < end_aligned; vaddr += PAGE_SIZE) {
        vmm_map_page(pml4, vaddr, curr_phys, flags);
        curr_phys += PAGE_SIZE;
    }
}

// ============================================================================
// 4. Dynamic Symbol Resolver
// ============================================================================

static uint64_t resolve_external_symbol(const char *name) {
    if (!name || name[0] == '\0') return 0;

    loaded_module_t *curr = global_module_list;
    while (curr) {
        if (curr->symtab && curr->strtab) {
            for (uint32_t i = 1; i < curr->sym_count; i++) {
                Elf64_Sym *s = &curr->symtab[i];
                if (s->st_name && s->st_shndx != 0) {
                    const char *sym_name = curr->strtab + s->st_name;
                    if (elf_strcmp(sym_name, name) == 0) {
                        return s->st_value + curr->load_bias;
                    }
                }
            }
        }
        curr = curr->next;
    }

    printk(LOG_ERROR, "[SYMBOL RESOLVER UNRESOLVED] Failed to resolve symbol: '%s'\n", name);
    return 0;
}

// ============================================================================
// 5. Relocation Processing Engine
// ============================================================================

static void apply_relocations(const char *module_name, Elf64_Rela *relas, uint64_t rela_sz, uint64_t rela_ent,
                              Elf64_Sym *symtab, const char *strtab,
                              uint64_t vma_base, void *load_vma, size_t allocated_size, int is_pic, uint64_t load_bias) {
    if (!relas || rela_sz == 0 || rela_ent == 0) return;

    uint64_t n_relas = rela_sz / rela_ent;

    for (uint64_t j = 0; j < n_relas; j++) {
        Elf64_Rela *rel = (Elf64_Rela *)((uint8_t *)relas + (j * rela_ent));
        uint64_t type    = ELF64_R_TYPE(rel->r_info);
        uint64_t sym_idx = ELF64_R_SYM(rel->r_info);

        void *patch_ptr = elf_vaddr_to_ptr(rel->r_offset, vma_base, load_vma, is_pic);
        uint64_t *patch = (uint64_t *)patch_ptr;

        if (!patch_ptr || !is_valid_write_ptr(patch_ptr, load_vma, allocated_size)) {
            continue;
        }

        uint64_t sym_val = 0;
        size_t   sym_size = 0;
        void    *sym_src_ptr = NULL;
        const char *sym_name = "<LOCAL/NONE>";

        if (sym_idx) {
            if (!symtab || !strtab) continue;

            Elf64_Sym *sym = &symtab[sym_idx];
            sym_name = strtab + sym->st_name;
            sym_size = sym->st_size;

            if (sym->st_shndx == 0) {
                loaded_module_t *curr = global_module_list;
                while (curr) {
                    if (curr->symtab && curr->strtab) {
                        for (uint32_t i = 1; i < curr->sym_count; i++) {
                            Elf64_Sym *s = &curr->symtab[i];
                            if (s->st_name && s->st_shndx != 0) {
                                if (elf_strcmp(curr->strtab + s->st_name, sym_name) == 0) {
                                    sym_val = s->st_value + curr->load_bias;
                                    sym_src_ptr = (void*)sym_val;
                                    if (sym_size == 0) sym_size = s->st_size;
                                    break;
                                }
                            }
                        }
                    }
                    if (sym_val) break;
                    curr = curr->next;
                }
            } else {
                sym_val = is_pic ? (sym->st_value + load_bias) : (uint64_t)elf_vaddr_to_ptr(sym->st_value, vma_base, load_vma, 0);
                sym_src_ptr = (void*)sym_val;
            }
        }

        switch (type) {
            case R_X86_64_COPY:
                if (sym_src_ptr && patch_ptr) {
                    if (sym_size > 0) {
                        elf_memcpy(patch_ptr, sym_src_ptr, sym_size);
                    } else {
                        *(uintptr_t *)patch_ptr = (uintptr_t)sym_src_ptr;
                    }
                }
                break;

            case R_X86_64_RELATIVE:
                *patch = is_pic ? (load_bias + rel->r_addend) : (uint64_t)elf_vaddr_to_ptr(rel->r_addend, vma_base, load_vma, 0);
                break;

            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
                *patch = sym_val + rel->r_addend;
                break;

            case R_X86_64_32:
                *(uint32_t *)patch = (uint32_t)(sym_val + rel->r_addend);
                break;

            case R_X86_64_32S:
                *(int32_t *)patch = (int32_t)(sym_val + rel->r_addend);
                break;

            case R_X86_64_PC32:
                *(uint32_t *)patch = (uint32_t)(sym_val + rel->r_addend - (uintptr_t)patch);
                break;

            case R_X86_64_NONE:
            default:
                break;
        }
    }
}

// ============================================================================
// 6. Shared Library Dynamic Loader
// ============================================================================
#define MAX_LOADED_MODULES 32
static loaded_module_t static_modules[MAX_LOADED_MODULES];
static size_t static_module_count = 0;

static loaded_module_t* load_shared_library(const char *lib_name) {
    if (!lib_name) return NULL;

    for (size_t i = 0; i < static_module_count; i++) {
        if (elf_strcmp(static_modules[i].name, lib_name) == 0) {
            return &static_modules[i];
        }
    }

    if (static_module_count >= MAX_LOADED_MODULES) return NULL;

    loaded_module_t *mod = &static_modules[static_module_count++];
    elf_memset(mod, 0, sizeof(loaded_module_t));
    elf_strcpy(mod->name, lib_name);
    mod->state = MODULE_STATE_REGISTERED;

    mod->next = global_module_list;
    global_module_list = mod;

    char path[256];
    elf_strcpy(path, "/lib/");
    elf_strcpy(path + 5, lib_name);

    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    struct vfs_stat st;
    if (vfs_fstat(fd, &st) < 0) {
        vfs_free_fd(fd);
        return NULL;
    }

    void *file_buf = static_elf_alloc(st.st_size);
    if (!file_buf) {
        vfs_free_fd(fd);
        return NULL;
    }

    if (vfs_read(fd, file_buf, st.st_size, 0) != st.st_size) {
        vfs_free_fd(fd);
        return NULL;
    }
    vfs_free_fd(fd);

    uint64_t needed_mem = elf_needed_mem(file_buf);
    if (!needed_mem) return NULL;

    uint64_t aligned_mem_size = (needed_mem + PAGE_SIZE - 1ULL) & PAGE_MASK;

    void *phys_backstore_virt = static_phys_pool_alloc(aligned_mem_size);
    if (!phys_backstore_virt) return NULL;

    uint64_t lib_vma_start = user_lib_vma_bump;
    user_lib_vma_bump += aligned_mem_size;

    // Map user shared library pages into page tables
    page_table_t *pml4 = vmm_get_current_pml4();
    map_user_vma_region(pml4, lib_vma_start, phys_backstore_virt, aligned_mem_size);

    void *lib_memory = (void*)lib_vma_start;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_buf;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)file_buf + ehdr->e_phoff);

    uint64_t lib_vma_base = UINT64_MAX;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < lib_vma_base) {
            lib_vma_base = phdrs[i].p_vaddr;
        }
    }
    if (lib_vma_base == UINT64_MAX) lib_vma_base = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint8_t *dest = (uint8_t *)elf_vaddr_to_ptr(ph->p_vaddr, lib_vma_base, lib_memory, 1);
        uint8_t *src  = (uint8_t *)file_buf + ph->p_offset;

        if (ph->p_filesz > 0) elf_memcpy(dest, src, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) elf_memset(dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    mod->load_bias = (uint64_t)lib_memory;
    mod->vma_base = lib_vma_base;
    mod->load_vma = lib_memory;
    mod->allocated_size = needed_mem;
    mod->is_pic = 1;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            mod->dynamic_table = (Elf64_Dyn *)elf_vaddr_to_ptr(phdrs[i].p_vaddr, lib_vma_base, lib_memory, 1);
            break;
        }
    }

    if (mod->dynamic_table) {
        uint32_t *hash_table = NULL;
        for (Elf64_Dyn *d = mod->dynamic_table; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
                case DT_SYMTAB:
                    mod->symtab = (Elf64_Sym *)elf_vaddr_to_ptr(d->d_un.d_ptr, lib_vma_base, lib_memory, 1);
                    break;
                case DT_STRTAB:
                    mod->strtab = (const char *)elf_vaddr_to_ptr(d->d_un.d_ptr, lib_vma_base, lib_memory, 1);
                    break;
                case DT_HASH:
                    hash_table = (uint32_t *)elf_vaddr_to_ptr(d->d_un.d_ptr, lib_vma_base, lib_memory, 1);
                    break;
            }
        }
        if (hash_table) {
            mod->sym_count = hash_table[1];
        }
    }

    mod->state = MODULE_STATE_LOADED;
    return mod;
}

static void process_needed_dependencies(void) {
    for (size_t i = 0; i < static_module_count; i++) {
        loaded_module_t *mod = &static_modules[i];

        if (!mod->dynamic_table || !mod->strtab) continue;

        for (Elf64_Dyn *d = mod->dynamic_table; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED) {
                const char *child_name = mod->strtab + d->d_un.d_val;
                load_shared_library(child_name);
            }
        }
    }
}

// ============================================================================
// 7. Graph Relocation Pass
// ============================================================================

static void relocate_module_graph(loaded_module_t *mod) {
    if (!mod || mod->state == MODULE_STATE_RELOCATED) return;

    mod->state = MODULE_STATE_RELOCATED;
    if (!mod->dynamic_table) return;

    Elf64_Sym  *symtab   = mod->symtab;
    const char *strtab   = mod->strtab;
    Elf64_Rela *relas    = NULL;
    uint64_t   rela_sz   = 0;
    uint64_t   rela_ent  = 0;
    Elf64_Rela *jmprel   = NULL;
    uint64_t   jmprel_sz = 0;

    for (Elf64_Dyn *d = mod->dynamic_table; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_RELA:     relas     = (Elf64_Rela *)elf_vaddr_to_ptr(d->d_un.d_ptr, mod->vma_base, mod->load_vma, mod->is_pic); break;
            case DT_RELASZ:   rela_sz   = d->d_un.d_val; break;
            case DT_RELAENT:  rela_ent  = d->d_un.d_val; break;
            case DT_JMPREL:   jmprel    = (Elf64_Rela *)elf_vaddr_to_ptr(d->d_un.d_ptr, mod->vma_base, mod->load_vma, mod->is_pic); break;
            case DT_PLTRELSZ: jmprel_sz = d->d_un.d_val; break;
        }
    }

    apply_relocations(mod->name, relas, rela_sz, rela_ent, symtab, strtab, mod->vma_base, mod->load_vma, mod->allocated_size, mod->is_pic, mod->load_bias);
    apply_relocations(mod->name, jmprel, jmprel_sz, sizeof(Elf64_Rela), symtab, strtab, mod->vma_base, mod->load_vma, mod->allocated_size, mod->is_pic, mod->load_bias);
}

// ============================================================================
// 8. Public Probing & Core Main Loader
// ============================================================================

uint64_t elf_needed_mem(void *raw_elf_data) {
    if (!raw_elf_data) return 0;
    ElfLoadResult probe = load_elf(raw_elf_data, 0, 0);
    return probe.mem_size;
}

uint64_t elf_vaddr(void *raw_elf_data) {
    if (!raw_elf_data) return 0;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') return 0;

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

ElfLoadResult load_elf(void *raw_elf_data, uint64_t unused_phys_base, uint64_t load_vma) {
    (void)unused_phys_base;
    ElfLoadResult result = {0, 0, 0};

    if (!raw_elf_data) return result;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        return result;
    }

    int is_pic = (ehdr->e_type == ET_DYN);
    uint64_t load_bias = is_pic ? load_vma : 0;
    int probe_only = (load_vma == 0);

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return result;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)raw_elf_data + ehdr->e_phoff);

    uint64_t vma_base = UINT64_MAX;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < vma_base) {
            vma_base = phdrs[i].p_vaddr;
        }
    }
    if (vma_base == UINT64_MAX) vma_base = 0;

    // Calculate maximum VMA memory span
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t vma_offset = ph->p_vaddr - vma_base;
        uint64_t segment_end = vma_offset + ph->p_memsz;
        if (segment_end > result.mem_size) {
            result.mem_size = segment_end;
        }
    }

    if (probe_only) return result;

    // COPY ELF SEGMENTS DIRECTLY INTO THE KERNEL VIRTUAL STAGING BUFFER (load_vma)
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t vma_offset = ph->p_vaddr - vma_base;
        uint8_t *dest = (uint8_t *)load_vma + vma_offset;
        uint8_t *src  = (uint8_t *)raw_elf_data + ph->p_offset;

        if (ph->p_filesz > 0) {
            elf_memcpy(dest, src, ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            elf_memset(dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
        }
    }

    result.entry_point  = is_pic ? (ehdr->e_entry + load_bias) : (uint64_t)elf_vaddr_to_ptr(ehdr->e_entry, vma_base, (void*)load_vma, 0);
    result.virtual_addr = load_vma;

    Elf64_Dyn *dynamic_table = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynamic_table = (Elf64_Dyn *)elf_vaddr_to_ptr(phdrs[i].p_vaddr, vma_base, (void*)load_vma, is_pic);
            break;
        }
    }

    if (!dynamic_table) return result;

    Elf64_Sym  *symtab   = NULL;
    const char *strtab   = NULL;
    Elf64_Rela *relas    = NULL;
    uint64_t   rela_sz   = 0;
    uint64_t   rela_ent  = 0;
    Elf64_Rela *jmprel   = NULL;
    uint64_t   jmprel_sz = 0;

    for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab    = (Elf64_Sym *)elf_vaddr_to_ptr(d->d_un.d_ptr, vma_base, (void*)load_vma, is_pic); break;
            case DT_STRTAB:   strtab    = (const char *)elf_vaddr_to_ptr(d->d_un.d_ptr, vma_base, (void*)load_vma, is_pic); break;
            case DT_RELA:     relas     = (Elf64_Rela *)elf_vaddr_to_ptr(d->d_un.d_ptr, vma_base, (void*)load_vma, is_pic); break;
            case DT_RELASZ:   rela_sz   = d->d_un.d_val; break;
            case DT_RELAENT:  rela_ent  = d->d_un.d_val; break;
            case DT_JMPREL:   jmprel    = (Elf64_Rela *)elf_vaddr_to_ptr(d->d_un.d_ptr, vma_base, (void*)load_vma, is_pic); break;
            case DT_PLTRELSZ: jmprel_sz = d->d_un.d_val; break;
        }
    }

    for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED && strtab) {
            const char *lib_name = strtab + d->d_un.d_val;
            load_shared_library(lib_name);
        }
    }

    process_needed_dependencies();

    apply_relocations("MAIN_EXEC", relas, rela_sz, rela_ent, symtab, strtab, vma_base, (void*)load_vma, result.mem_size, is_pic, load_bias);
    apply_relocations("MAIN_EXEC", jmprel, jmprel_sz, sizeof(Elf64_Rela), symtab, strtab, vma_base, (void*)load_vma, result.mem_size, is_pic, load_bias);

    loaded_module_t *curr = global_module_list;
    while (curr) {
        relocate_module_graph(curr);
        curr = curr->next;
    }

    return result;
}