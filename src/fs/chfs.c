#include <fs/chfs.h>
#include <stddef.h>
#include <fs/gpt.h>
#include <hals/ahci.h>
#include <string.h>
#include <drivers/fb.h>

#define SECTOR_SIZE 512
#define PAGE_SIZE 4096

extern void* pmm_alloc_pages(int order);
extern void pmm_free_pages(void* ptr, int order);

// Helper to convert virtual addresses ONLY for pmm_alloc_pages in the HHDM
static inline uint64_t VA_TO_PA(void* va) {
    return (uint64_t)va - HHDM_OFFSET;
}

// --------------------------------------------------------------------------
// SAFE DMA WRAPPERS - Solves Virtual Heap Translation Corruptions
// --------------------------------------------------------------------------
static int chfs_dma_read(ahci_device_t* dev, uint64_t lba, uint32_t count, void* dest) {
    uint32_t bytes = count * SECTOR_SIZE;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while ((1 << order) < pages) order++;
    
    void* dma_buf = pmm_alloc_pages(order);
    if (!dma_buf) return 0;
    
    uint64_t phys = VA_TO_PA(dma_buf);
    int status = ahci_read_sectors(dev, lba, count, phys);
    if (status == 1) {
        memcpy(dest, dma_buf, bytes);
    }
    pmm_free_pages(dma_buf, order);
    return status;
}

static int chfs_dma_write(ahci_device_t* dev, uint64_t lba, uint32_t count, void* src) {
    uint32_t bytes = count * SECTOR_SIZE;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while ((1 << order) < pages) order++;
    
    void* dma_buf = pmm_alloc_pages(order);
    if (!dma_buf) return 0;
    
    memcpy(dma_buf, src, bytes);
    uint64_t phys = VA_TO_PA(dma_buf);
    int status = ahci_write_sectors(dev, lba, count, phys);
    pmm_free_pages(dma_buf, order);
    return status;
}
// --------------------------------------------------------------------------

typedef struct {
    CHFS_HDR* hdr;
    CHFS_FHDR* fhdr;
    uint32_t* in_indexing;
    CHFS_JRN* journal;
    uint32_t* jn_indexing;
    uint16_t file_count;
    uint32_t inode_count;
    uint64_t journal_count;
    char vol_name[36];
    uint32_t free_fhdr;
    uint32_t free_inodes;
    ahci_device_t dev;
    uint64_t start_lba; 
} cached;

cached metadata[32];
int lastdisk = 0;

void init_chfs() {
    printk(LOG_TRACE, "[CHFS] init_chfs: Initializing global cache space arrays\n");
    memset(metadata, 0, sizeof(metadata));
}

int add_partition(volume_t* vol) {
    printk(LOG_TRACE, "[CHFS] add_partition: Attaching new partition volume interface\n");
    if (!vol || !vol->drive || lastdisk >= 32) return -1;
    int disk = lastdisk++;
    
    CHFS_HDR* hdr = kmalloc(SECTOR_SIZE);
    if (!hdr) { lastdisk--; return -1; }
    
    if (chfs_dma_read(vol->drive, vol->start_lba, 1, hdr) != 1) {
        kfree(hdr); lastdisk--; return -1;
    }
    
    if (hdr->magic != 0xDEADBEEF) {
        printk(LOG_TRACE, "[CHFS] add_partition: Aborted - Expected signature 0xDEADBEEF, found 0x%x\n", (unsigned int)hdr->magic);
        kfree(hdr); lastdisk--; return -1;
    }
    
    size_t fhdr_sz = (hdr->file_count > 0 ? hdr->file_count : 64) * sizeof(CHFS_FHDR);
    size_t idx_sz  = (hdr->inode_count > 0 ? hdr->inode_count : 256) * sizeof(uint32_t);
    size_t idxjn_sz  = (hdr->journal_count > 0 ? hdr->journal_count : 64) * sizeof(uint32_t);
    
    size_t fhdr_sectors = (fhdr_sz + SECTOR_SIZE - 1) / SECTOR_SIZE;
    size_t idx_sectors  = (idx_sz + SECTOR_SIZE - 1) / SECTOR_SIZE;
    size_t idxjn_sectors  = (idxjn_sz + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    if (fhdr_sectors == 0) fhdr_sectors = 1;
    if (idx_sectors == 0) idx_sectors = 1;
    if (idxjn_sectors == 0) idxjn_sectors = 1;

    CHFS_FHDR* fhdr_table = kmalloc(fhdr_sectors * SECTOR_SIZE);
    uint32_t* idx_table   = kmalloc(idx_sectors * SECTOR_SIZE);
    uint32_t* jn_idx_table   = kmalloc(idxjn_sectors * SECTOR_SIZE);
    
    if (!fhdr_table || !idx_table || !jn_idx_table) {
        if (fhdr_table) kfree(fhdr_table);
        if (idx_table) kfree(idx_table);
        if (jn_idx_table) kfree(jn_idx_table);
        kfree(hdr); lastdisk--; return -1;
    }

    memset(fhdr_table, 0, fhdr_sectors * SECTOR_SIZE);
    memset(idx_table, 0, idx_sectors * SECTOR_SIZE);
    memset(jn_idx_table, 0, idxjn_sectors * SECTOR_SIZE);

    uint64_t fhdr_lba = vol->start_lba + (hdr->relative_header_table_addr / SECTOR_SIZE);
    chfs_dma_read(vol->drive, fhdr_lba, fhdr_sectors, fhdr_table);
    
    uint64_t idx_lba = vol->start_lba + (hdr->relative_indexing_table / SECTOR_SIZE);
    chfs_dma_read(vol->drive, idx_lba, idx_sectors, idx_table);

    uint64_t idxjn_lba = vol->start_lba + (hdr->relative_journal_indexing / SECTOR_SIZE);
    chfs_dma_read(vol->drive, idxjn_lba, idxjn_sectors, jn_idx_table);
    
    metadata[disk].hdr           = hdr;
    metadata[disk].fhdr          = fhdr_table;
    metadata[disk].in_indexing   = idx_table;
    metadata[disk].jn_indexing   = jn_idx_table;
    metadata[disk].free_fhdr     = hdr->relative_free_fhdr;
    metadata[disk].free_inodes   = hdr->relative_free_inodes;
    metadata[disk].file_count    = hdr->file_count;
    metadata[disk].inode_count   = hdr->inode_count;
    metadata[disk].journal_count = hdr->journal_count;
    metadata[disk].start_lba     = vol->start_lba;
    metadata[disk].dev           = *vol->drive;
    
    strncpy(metadata[disk].vol_name, vol->name, sizeof(metadata[disk].vol_name) - 1);
    
    return disk;
}

static CHFS_IN* read_inode_from_disk(int disk, uint32_t raw_inode_offset) {
    if (disk < 0 || disk >= lastdisk) return NULL;
    uint64_t total_byte_offset = metadata[disk].hdr->relative_inode_table_addr + raw_inode_offset;
    uint64_t absolute_lba = metadata[disk].start_lba + (total_byte_offset / SECTOR_SIZE);
    
    uint32_t sectors_to_read = 9; 
    void* io_buffer = kmalloc(sectors_to_read * SECTOR_SIZE);
    if (!io_buffer) return NULL;
    
    if (chfs_dma_read(&metadata[disk].dev, absolute_lba, sectors_to_read, io_buffer) != 1) {
        kfree(io_buffer); return NULL;
    }
    
    uint32_t structural_misalignment = total_byte_offset % SECTOR_SIZE;
    return (CHFS_IN*)((uintptr_t)io_buffer + structural_misalignment);
}

static void write_inode_to_disk(int disk, uint32_t raw_inode_offset, CHFS_IN* inode_ptr) {
    if (disk < 0 || disk >= lastdisk || !inode_ptr) return;
    uint64_t total_byte_offset = metadata[disk].hdr->relative_inode_table_addr + raw_inode_offset;
    uint64_t absolute_lba = metadata[disk].start_lba + (total_byte_offset / SECTOR_SIZE);
    uint32_t structural_misalignment = total_byte_offset % SECTOR_SIZE;
    
    uint32_t sectors_to_write = 9;
    void* io_buffer_base = (void*)((uintptr_t)inode_ptr - structural_misalignment);
    
    chfs_dma_write(&metadata[disk].dev, absolute_lba, sectors_to_write, io_buffer_base);
    kfree(io_buffer_base);
}

int read_chfs(int disk, char* path, void* buffer, int offset, int count) {
    if (disk < 0 || disk >= lastdisk) return -1;
    int fhdr_index = -1;
    for (int i = 0; i < metadata[disk].file_count; i++) {
        if (!strcmp(metadata[disk].fhdr[i].path, path)) { fhdr_index = i; break; }
    }
    if (fhdr_index == -1) return -1;

    CHFS_FHDR fhdr = metadata[disk].fhdr[fhdr_index];
    void* buffer_internal = kmalloc(fhdr.size ? fhdr.size : 1);
    if (!buffer_internal) return -1;
    int internal_offset = 0;

    for (int i = 0; i < (int)fhdr.inode_count; i++) {
        uint32_t raw_inode_addr = metadata[disk].in_indexing[fhdr.inode_indexes[i]];
        CHFS_IN* inode = read_inode_from_disk(disk, raw_inode_addr);
        if (!inode) { kfree(buffer_internal); return -1; }

        extern size_t buffer_crop_to_output(const void* src, size_t src_total_sz, void* dest, size_t offset, size_t count);
        buffer_crop_to_output(inode->payload, inode->size, (void*)((uintptr_t)buffer_internal + internal_offset), 0, inode->size);
        
        internal_offset += inode->size;
        uint32_t structural_misalignment = (metadata[disk].hdr->relative_inode_table_addr + raw_inode_addr) % SECTOR_SIZE;
        kfree((void*)((uintptr_t)inode - structural_misalignment));
    }

    extern size_t buffer_crop_to_output(const void* src, size_t src_total_sz, void* dest, size_t offset, size_t count);
    int result = buffer_crop_to_output(buffer_internal, fhdr.size, buffer, offset, count);
    kfree(buffer_internal);
    return result;
}

void write_fhdr_to_disk(int disk, uint32_t fhdr_index, CHFS_FHDR* fhdr_ptr) {
    if (disk < 0 || disk >= lastdisk || !fhdr_ptr) return;
    uint64_t total_byte_offset = metadata[disk].hdr->relative_header_table_addr + (fhdr_index * sizeof(CHFS_FHDR));
    uint64_t absolute_lba = metadata[disk].start_lba + (total_byte_offset / SECTOR_SIZE);
    uint32_t structural_misalignment = total_byte_offset % SECTOR_SIZE;
    
    size_t alloc_sectors = (sizeof(CHFS_FHDR) + structural_misalignment + SECTOR_SIZE - 1) / SECTOR_SIZE;
    void* io_buffer = kmalloc(alloc_sectors * SECTOR_SIZE);
    if (!io_buffer) return;

    chfs_dma_read(&metadata[disk].dev, absolute_lba, alloc_sectors, io_buffer);
    memcpy((void*)((uintptr_t)io_buffer + structural_misalignment), fhdr_ptr, sizeof(CHFS_FHDR));
    chfs_dma_write(&metadata[disk].dev, absolute_lba, alloc_sectors, io_buffer);
    kfree(io_buffer);
}

int write_chfs(int disk, char* path, void* buffer, size_t size) {
    if (disk < 0 || disk >= lastdisk) return -1;
    int fhdr_index = -1;
    for (int i = 0; i < metadata[disk].file_count; i++) {
        if (!strcmp(metadata[disk].fhdr[i].path, path)) { fhdr_index = i; break; }
    }
    if (fhdr_index == -1) return -1;

    CHFS_FHDR fhdr = metadata[disk].fhdr[fhdr_index];
    uint64_t quotient = size / 4096;
    uint64_t remainder = size % 4096;

    if ((quotient == 0 && remainder != 0) || (quotient == 1 && remainder == 0)) { 
        uint32_t raw_inode_addr = metadata[disk].in_indexing[fhdr.inode_indexes[0]];
        CHFS_IN* inode = read_inode_from_disk(disk, raw_inode_addr);
        if (!inode) return -1;
        memcpy(inode->payload, buffer, size);
        inode->size = size;
        write_inode_to_disk(disk, raw_inode_addr, inode);
        metadata[disk].fhdr[fhdr_index].size = size;
        write_fhdr_to_disk(disk, fhdr_index, &metadata[disk].fhdr[fhdr_index]);
        return 0;
    } else {
        for (int i = 0; i < (int)quotient; i++) {
            if (i >= (int)fhdr.inode_count) return -1;
            uint32_t raw_inode_addr = metadata[disk].in_indexing[fhdr.inode_indexes[i]];
            CHFS_IN* inode = read_inode_from_disk(disk, raw_inode_addr);
            if (!inode) return -1;
            uintptr_t addr = (uintptr_t)buffer + (i * 4096);
            memcpy(inode->payload, (void*)addr, 4096);
            inode->size = 4096;
            write_inode_to_disk(disk, raw_inode_addr, inode);
        }
        if (remainder != 0) {
            if ((int)quotient >= (int)fhdr.inode_count) return -1;
            uint32_t raw_inode_addr = metadata[disk].in_indexing[fhdr.inode_indexes[quotient]];
            CHFS_IN* inode = read_inode_from_disk(disk, raw_inode_addr);
            if (!inode) return -1;
            uintptr_t addr = (uintptr_t)buffer + (quotient * 4096);
            memcpy(inode->payload, (void*)addr, remainder);
            inode->size = remainder;
            write_inode_to_disk(disk, raw_inode_addr, inode);
        }
        metadata[disk].fhdr[fhdr_index].size = size;
        write_fhdr_to_disk(disk, fhdr_index, &metadata[disk].fhdr[fhdr_index]);
        return 0;
    }
}

int create_chfs(int disk, char* path) {
    if (disk < 0 || disk >= lastdisk || metadata[disk].file_count >= 64) return -1;

    uint32_t fhdr_idx = metadata[disk].file_count;
    CHFS_FHDR* new_fhdr = &metadata[disk].fhdr[fhdr_idx];
    memset(new_fhdr, 0, sizeof(CHFS_FHDR));
    strncpy(new_fhdr->path, path, sizeof(new_fhdr->path) - 1);
    
    new_fhdr->inode_count = 1;
    new_fhdr->size = 0;
    new_fhdr->attr = ATTR_FILE;

    uint32_t node_idx = metadata[disk].inode_count;
    if (node_idx >= 256) return -1;
    
    new_fhdr->inode_indexes[0] = node_idx;
    metadata[disk].in_indexing[node_idx] = metadata[disk].free_inodes;

    CHFS_IN* clean_node = kmalloc(9 * SECTOR_SIZE);
    if (!clean_node) return -1;
    memset(clean_node, 0, 9 * SECTOR_SIZE);
    clean_node->size = 0;
    clean_node->mode = 0644;

    write_inode_to_disk(disk, metadata[disk].free_inodes, clean_node);
    
    metadata[disk].free_inodes += (9 * SECTOR_SIZE);
    metadata[disk].inode_count++;
    metadata[disk].file_count++;

    write_fhdr_to_disk(disk, fhdr_idx, new_fhdr);

    uint64_t idx_table_offset = metadata[disk].hdr->relative_indexing_table;
    uint64_t idx_lba = metadata[disk].start_lba + (idx_table_offset / SECTOR_SIZE);
    size_t idx_sectors = (256 * sizeof(uint32_t) + SECTOR_SIZE - 1) / SECTOR_SIZE;
    chfs_dma_write(&metadata[disk].dev, idx_lba, idx_sectors, metadata[disk].in_indexing);

    metadata[disk].hdr->file_count = metadata[disk].file_count;
    metadata[disk].hdr->inode_count = metadata[disk].inode_count;
    metadata[disk].hdr->relative_free_inodes = metadata[disk].free_inodes;
    chfs_dma_write(&metadata[disk].dev, metadata[disk].start_lba, 1, metadata[disk].hdr);

    return 0;
}

int format_chfs(volume_t* vol) {
    if (!vol || !vol->drive) return -1;

    // Use absolute physical page allocated by PMM to clear disk
    void* dma_zero = pmm_alloc_pages(0);
    if (!dma_zero) return -1;
    memset(dma_zero, 0, PAGE_SIZE);
    uint64_t zero_phys = VA_TO_PA(dma_zero);

    CHFS_HDR master_hdr;
    memset(&master_hdr, 0, sizeof(CHFS_HDR));
    master_hdr.magic = 0xDEADBEEF;
    master_hdr.version = 6;
    master_hdr.file_count = 0;
    master_hdr.inode_count = 0;
    master_hdr.journal_count = 0;
    master_hdr.relative_header_table_addr = 512;
    master_hdr.relative_indexing_table    = 512 + (64 * sizeof(CHFS_FHDR));
    master_hdr.relative_inode_table_addr   = master_hdr.relative_indexing_table + (256 * sizeof(uint32_t));
    master_hdr.relative_journal_indexing  = master_hdr.relative_inode_table_addr + (256 * sizeof(CHFS_IN));
    master_hdr.relative_journal           = master_hdr.relative_journal_indexing + (64 * sizeof(uint32_t));
    master_hdr.relative_free_fhdr         = 0;
    master_hdr.relative_free_inodes       = 0;

    uint32_t total_sectors_to_clear = (master_hdr.relative_journal + (64 * sizeof(CHFS_JRN)) + SECTOR_SIZE - 1) / SECTOR_SIZE;

    int last_logged_percentage = -1;

    for (uint32_t i = 1; i < total_sectors_to_clear; i++) {
        ahci_write_sectors(vol->drive, vol->start_lba + i, 1, zero_phys);
        int current_percentage = (int)(((uint64_t)i * 100) / total_sectors_to_clear);
        if (current_percentage % 5 == 0 && current_percentage != last_logged_percentage) {
            printk(LOG_TRACE, "[CHFS] format_chfs: Formatting progress at %d%%\n", current_percentage);
            last_logged_percentage = current_percentage;
        }
    }
    pmm_free_pages(dma_zero, 0);

    // Write master header using new safe wrapper
    void* header_block = kmalloc(SECTOR_SIZE);
    memset(header_block, 0, SECTOR_SIZE);
    memcpy(header_block, &master_hdr, sizeof(CHFS_HDR));
    int status = chfs_dma_write(vol->drive, vol->start_lba, 1, header_block);
    kfree(header_block);

    if (status != 1) return -1;

    // Verify master header using safe wrapper
    void* readback_block = kmalloc(SECTOR_SIZE);
    memset(readback_block, 0, SECTOR_SIZE);
    int read_status = chfs_dma_read(vol->drive, vol->start_lba, 1, readback_block);
    
    if (read_status != 1) {
        kfree(readback_block);
        return -1;
    }

    CHFS_HDR* verify_hdr = (CHFS_HDR*)readback_block;
    if (verify_hdr->magic != 0xDEADBEEF || verify_hdr->version != 6) {
        printk(LOG_TRACE, "[CHFS] format_chfs: Readback mismatch! Magic: 0x%x, Version: %d\n", 
               (unsigned int)verify_hdr->magic, (int)verify_hdr->version);
        kfree(readback_block);
        return -1;
    }

    kfree(readback_block);
    printk(LOG_TRACE, "[CHFS] format_chfs: File system generation finalized completely.\n");
    return 0;
}