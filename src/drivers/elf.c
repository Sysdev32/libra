#include <stdint.h>
#include <stddef.h>

#define EI_NIDENT 16
#define HHDM_OFFSET 0xffff800000000000ULL 

// ============================================================================
// ELF64 Specification Structures
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

#define ET_EXEC    2   
#define ET_DYN     3   
#define PT_LOAD    1
#define SHT_SYMTAB 2
#define SHT_RELA   4
#define SHT_DYNSYM 11

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffffL)

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6   
#define R_X86_64_JUMP_SLOT 7   
#define R_X86_64_RELATIVE  8

typedef struct {
    uint64_t entry_point;
    uint64_t virtual_addr;
    uint64_t mem_size;
} ElfLoadResult;

// ============================================================================
// Core Memory Utilities
// ============================================================================

static int local_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void local_memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static void local_memset(void* dest, int val, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
}

// ============================================================================
// Runtime Address Space Matcher
// ============================================================================

static uint64_t find_internal_symbol(Elf64_Sym* sym_table, uint64_t sym_count, const char* str_table, const char* name, uint64_t load_bias) {
    if (!sym_table || !str_table) return 0;

    for (uint64_t i = 0; i < sym_count; i++) {
        Elf64_Sym* sym = &sym_table[i];
        if (sym->st_name != 0 && sym->st_shndx != 0) {
            if (local_strcmp(str_table + sym->st_name, name) == 0) {
                return sym->st_value + load_bias;
            }
        }
    }
    return 0;
}

// ============================================================================
// Fully Rewritten ELF Loader
// ============================================================================

ElfLoadResult load_elf(void* raw_elf_data, uint64_t physical_base_arg, uint64_t expected_vma_base) {
    ElfLoadResult result = {0, 0, 0};
    Elf64_Ehdr* elf_header = (Elf64_Ehdr*)raw_elf_data;

    // Phase 1: Header Validation
    if (elf_header->e_ident[0] != 0x7F || elf_header->e_ident[1] != 'E' ||
        elf_header->e_ident[2] != 'L' || elf_header->e_ident[3] != 'F') {
        return result; 
    }

    uint64_t load_bias = 0;
    if (elf_header->e_type == ET_DYN) {
        load_bias = expected_vma_base;
    }

    result.entry_point  = elf_header->e_entry + load_bias;
    result.virtual_addr = expected_vma_base;
    result.mem_size     = 0; 

    // Phase 2: Copy Program Segments
    if (elf_header->e_phoff != 0 && elf_header->e_phnum > 0) {
        Elf64_Phdr* program_headers = (Elf64_Phdr*)((uint8_t*)raw_elf_data + elf_header->e_phoff);
        
        for (uint16_t i = 0; i < elf_header->e_phnum; i++) {
            Elf64_Phdr* p_header = &program_headers[i];

            if (p_header->p_type == PT_LOAD) {
                uint64_t vma_offset = 0;
                
                if (elf_header->e_type == ET_DYN) {
                    vma_offset = p_header->p_vaddr;
                } else {
                    if (p_header->p_vaddr >= expected_vma_base) {
                        vma_offset = p_header->p_vaddr - expected_vma_base;
                    } else {
                        vma_offset = p_header->p_vaddr;
                    }
                }

                uint8_t* dest_hhdm = (uint8_t*)(physical_base_arg + vma_offset + HHDM_OFFSET);
                uint8_t* src_data  = (uint8_t*)raw_elf_data + p_header->p_offset;

                // Track memory size requirements relative to the binary layout origin
                uint64_t segment_end = vma_offset + p_header->p_memsz;
                if (segment_end > result.mem_size) {
                    result.mem_size = segment_end;
                }

                // Copy text / data segment payload
                if (p_header->p_filesz > 0) {
                    local_memcpy(dest_hhdm, src_data, p_header->p_filesz);
                }
                
                // Zero out the remaining trailing space allocated for .bss
                if (p_header->p_memsz > p_header->p_filesz) {
                    local_memset(dest_hhdm + p_header->p_filesz, 0, p_header->p_memsz - p_header->p_filesz);
                }
            }
        }
    }

    // Phase 3: Dynamic Relocation Parsing
    Elf64_Sym* sym_table = NULL;
    const char* str_table = NULL;
    uint64_t sym_count = 0;

    if (elf_header->e_shoff != 0 && elf_header->e_shnum > 0) {
        Elf64_Shdr* section_headers = (Elf64_Shdr*)((uint8_t*)raw_elf_data + elf_header->e_shoff);

        for (uint16_t i = 0; i < elf_header->e_shnum; i++) {
            Elf64_Shdr* sh = &section_headers[i];
            if ((sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) && sh->sh_entsize > 0) {
                sym_table = (Elf64_Sym*)((uint8_t*)raw_elf_data + sh->sh_offset);
                sym_count = sh->sh_size / sh->sh_entsize;
                
                Elf64_Shdr* str_sh = &section_headers[sh->sh_link];
                str_table = (const char*)((uint8_t*)raw_elf_data + str_sh->sh_offset);
                break;
            }
        }

        // Phase 4: Patching Relocations
        for (uint16_t i = 0; i < elf_header->e_shnum; i++) {
            Elf64_Shdr* s_header = &section_headers[i];

            if (s_header->sh_type == SHT_RELA && s_header->sh_entsize > 0) {
                Elf64_Rela* rela_table = (Elf64_Rela*)((uint8_t*)raw_elf_data + s_header->sh_offset);
                uint64_t entries = s_header->sh_size / s_header->sh_entsize;

                for (uint64_t j = 0; j < entries; j++) {
                    Elf64_Rela* rel = &rela_table[j];
                    uint64_t type = ELF64_R_TYPE(rel->r_info);
                    uint64_t sym_idx = ELF64_R_SYM(rel->r_info);

                    uint64_t patch_vma = rel->r_offset;
                    uint64_t patch_offset = 0;

                    if (elf_header->e_type == ET_DYN) {
                        patch_offset = rel->r_offset;
                    } else {
                        if (patch_vma >= expected_vma_base) {
                            patch_offset = patch_vma - expected_vma_base;
                        } else {
                            patch_offset = patch_vma;
                        }
                    }

                    uint64_t* patch_target = (uint64_t*)(physical_base_arg + patch_offset + HHDM_OFFSET);
                    uint64_t symbol_address = 0;

                    if (sym_idx != 0 && sym_table && str_table) {
                        Elf64_Sym* sym = &sym_table[sym_idx];
                        if (sym->st_shndx == 0) { 
                            const char* sym_name = str_table + sym->st_name;
                            symbol_address = find_internal_symbol(sym_table, sym_count, str_table, sym_name, load_bias);
                        } else {
                            symbol_address = sym->st_value + load_bias;
                        }
                    }

                    switch (type) {
                        case R_X86_64_RELATIVE:
                            *patch_target = load_bias + rel->r_addend;
                            break;
                        case R_X86_64_64:
                        case R_X86_64_GLOB_DAT:   
                        case R_X86_64_JUMP_SLOT:  
                            *patch_target = symbol_address + rel->r_addend;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }

    return result;
}