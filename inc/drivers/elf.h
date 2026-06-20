#pragma once
#include <stdint.h>
typedef struct {
    uint64_t entry_point;
    uint64_t virtual_addr;
    uint64_t mem_size;
} ElfLoadResult;
ElfLoadResult load_elf(void* raw_elf_data, uint64_t physical_base_arg, uint64_t expected_vma_base);