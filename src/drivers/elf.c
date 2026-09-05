#include <stdint.h>
#include <stddef.h>
#include <drivers/elf.h>
#include <fs/mnt.h>
#include <drivers/alloc.h>

#ifndef DT_HASH
#define DT_HASH 4
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#ifndef PTE_USER
#define PTE_USER (1ULL << 2)
#endif

#ifndef PTE_WRITABLE
#define PTE_WRITABLE (1ULL << 1)
#endif

#ifndef HHDM_OFFSET
#define HHDM_OFFSET 0xffff800000000000ULL
#endif

// Forward Declarations for VMM integration
extern uint64_t vmm_virt_to_phys(page_table_t *pml4, uint64_t vma);
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern void *pmm_alloc_pages(int order);

// ============================================================================
// Global Library Tracking
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
// Internal Utilities
// ============================================================================

static int elf_strcmp(const char *a, const char *b) {
    if (!a || !b) return -1;
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void elf_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void elf_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
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

static void *user_vma_to_hhdm(page_table_t *pml4, uint64_t vma) {
    if (!pml4) return NULL;
    uint64_t phys = vmm_virt_to_phys(pml4, vma);
    if (!phys) return NULL;
    return (void *)(phys + HHDM_OFFSET);
}

uint64_t elf_needed_mem(void *raw_elf_data) {
    ElfLoadResult probe = load_elf(raw_elf_data, NULL, 0);
    return probe.mem_size;
}

uint64_t elf_vaddr(void *raw_elf_data) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)raw_elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
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

// ============================================================================
// Symbol Resolution & Shared Library Handling
// ============================================================================

static uint64_t resolve_external_symbol(const char *name) {
    loaded_module_t *curr = global_module_list;

    while (curr) {
        if (curr->symtab && curr->strtab) {
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

static void load_shared_library(const char *lib_name, page_table_t *user_pml4) {
    loaded_module_t *curr = global_module_list;
    while (curr) {
        if (elf_strcmp(curr->name, lib_name) == 0) return;
        curr = curr->next;
    }

    char path[256];
    elf_strcpy(path, "/lib/");

    size_t prefix_len = 5;
    size_t name_len = elf_strlen(lib_name);
    if (prefix_len + name_len < 256) {
        elf_strcpy(path + prefix_len, lib_name);
    } else {
        return;
    }

    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return;

    struct vfs_stat st;
    if (vfs_fstat(fd, &st) < 0) {
        vfs_free_fd(fd);
        return;
    }

    void *file_buf = kmalloc(st.st_size);
    if (!file_buf) {
        vfs_free_fd(fd);
        return;
    }

    vfs_read(fd, file_buf, st.st_size, 0);
    vfs_free_fd(fd);

    uint64_t needed_mem = elf_needed_mem(file_buf);
    if (needed_mem == 0) {
        kfree(file_buf);
        return;
    }

    void *lib_memory = kmalloc(needed_mem);
    if (!lib_memory) {
        kfree(file_buf);
        return;
    }

    uint64_t load_vma = (uint64_t)lib_memory;

    /* Recursively map library binary via load_elf into active memory space */
    load_elf(file_buf, user_pml4, load_vma);

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_buf;
    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)file_buf + ehdr->e_phoff);

    Elf64_Dyn *dynamic_table = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
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
                mod->sym_count = hashtab ? hashtab[1] : 8192;
                mod->next = global_module_list;
                global_module_list = mod;
            }
        }
    }

    kfree(file_buf);
}

// ============================================================================
// Core ELF Loader Function
// ============================================================================

ElfLoadResult load_elf(void *raw_elf_data, page_table_t *user_pml4, uint64_t load_vma) {
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
    int probe_only = (user_pml4 == NULL);

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

    /* Step 1: Map and load PT_LOAD segments */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t phys_offset = ph->p_vaddr - vma_base;
        uint64_t segment_end = phys_offset + ph->p_memsz;
        if (segment_end > result.mem_size) result.mem_size = segment_end;

        if (probe_only) continue;

        uint8_t *src = (uint8_t *)raw_elf_data + ph->p_offset;
        uint64_t copied = 0;
        uint64_t target_vma = ph->p_vaddr + load_bias;

        while (copied < ph->p_memsz) {
            uint64_t cur_vma = target_vma + copied;
            uint64_t page_off = cur_vma % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - page_off;
            if (chunk > (ph->p_memsz - copied)) chunk = ph->p_memsz - copied;

            uint8_t *dest = (uint8_t *)user_vma_to_hhdm(user_pml4, cur_vma);
            if (!dest) {
                void *page_virt = pmm_alloc_pages(0);
                if (!page_virt) return (ElfLoadResult){0, 0, 0};

                uint64_t page_phys = (uint64_t)page_virt - HHDM_OFFSET;
                vmm_map_page(user_pml4, cur_vma & ~(PAGE_SIZE - 1), page_phys, PTE_USER | PTE_WRITABLE);
                dest = (uint8_t *)user_vma_to_hhdm(user_pml4, cur_vma);
            }

            if (copied < ph->p_filesz) {
                size_t copy_bytes = chunk;
                if (copied + copy_bytes > ph->p_filesz) {
                    copy_bytes = ph->p_filesz - copied;
                }
                elf_memcpy(dest, src + copied, copy_bytes);

                if (copy_bytes < chunk) {
                    elf_memset(dest + copy_bytes, 0, chunk - copy_bytes);
                }
            } else {
                elf_memset(dest, 0, chunk);
            }

            copied += chunk;
        }
    }

    result.entry_point  = ehdr->e_entry + load_bias;
    result.virtual_addr = load_vma;

    if (probe_only) return result;

    /* Step 2: Locate Dynamic Section and resolve dependencies */
    Elf64_Dyn *dynamic_table = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynamic_table = (Elf64_Dyn *)user_vma_to_hhdm(user_pml4, phdrs[i].p_vaddr + load_bias);
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
            case DT_SYMTAB: symtab = (Elf64_Sym *)user_vma_to_hhdm(user_pml4, d->d_un.d_ptr + load_bias); break;
            case DT_STRTAB: strtab = (const char *)user_vma_to_hhdm(user_pml4, d->d_un.d_ptr + load_bias); break;
            case DT_RELA:   relas  = (Elf64_Rela *)user_vma_to_hhdm(user_pml4, d->d_un.d_ptr + load_bias); break;
            case DT_RELASZ: rela_sz = d->d_un.d_val; break;
            case DT_RELAENT:rela_ent = d->d_un.d_val; break;
        }
    }

    for (Elf64_Dyn *d = dynamic_table; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED && strtab) {
            const char *lib_name = strtab + d->d_un.d_val;
            load_shared_library(lib_name, user_pml4);
        }
    }

    /* Step 3: Perform dynamic relocations */
    if (relas && rela_sz > 0 && rela_ent > 0) {
        uint64_t n_relas = rela_sz / rela_ent;

        for (uint64_t j = 0; j < n_relas; j++) {
            Elf64_Rela *rel = &relas[j];
            uint64_t type    = ELF64_R_TYPE(rel->r_info);
            uint64_t sym_idx = ELF64_R_SYM(rel->r_info);

            uint64_t *patch = (uint64_t *)user_vma_to_hhdm(user_pml4, rel->r_offset + load_bias);
            if (!patch) continue;

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