#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <hals/virtio/virtio_gpu.h>
#include <drivers/fb.h>

extern bool get_physical_address(uint64_t virt_addr, uint64_t *out_phys);
extern void *pmm_alloc_pages(int order);

// Explicitly packed command structure for VirtIO-GPU
struct virtio_gpu_resource_attach_backing_req {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));

// Dynamic allocated, page-aligned buffer for DMA responses
static struct virtio_gpu_ctrl_hdr *g_res_buffer = NULL;

static struct virtio_gpu_ctrl_hdr *get_response_buffer(void) {
    if (!g_res_buffer) {
        g_res_buffer = (struct virtio_gpu_ctrl_hdr *)pmm_alloc_pages(0);
    }
    if (g_res_buffer) {
        memset(g_res_buffer, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    }
    return g_res_buffer;
}

bool virtio_gpu_init(virtio_gpu_device_t *gpu, virtio_device_t *vdev, uint32_t width, uint32_t height, void *fb_virt_addr) {
    if (!gpu || !vdev || !fb_virt_addr) return false;

    gpu->vdev = vdev;
    gpu->width = width;
    gpu->height = height;
    gpu->framebuffer = (uint32_t *)fb_virt_addr;
    gpu->resource_id = 1;

    struct virtio_gpu_ctrl_hdr *res = get_response_buffer();
    if (!res) {
        printk(LOG_TRACE, "[virtio-gpu] Failed to allocate response buffer via PMM!\n");
        return false;
    }

    // Resolve physical address of the framebuffer via Page Table Walker
    uint64_t fb_phys_addr = 0;
    if (!get_physical_address((uint64_t)gpu->framebuffer, &fb_phys_addr)) {
        printk(LOG_TRACE, "[virtio-gpu] Failed to resolve physical address for framebuffer 0x%p\n", gpu->framebuffer);
        return false;
    }

    printk(LOG_TRACE, "[virtio-gpu] Initializing GPU resource %u (%ux%u) Phys FB: 0x%lx\n",
           gpu->resource_id, gpu->width, gpu->height, fb_phys_addr);

    // Step 1: Create 2D Resource
    struct virtio_gpu_resource_create_2d create_req = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = gpu->resource_id,
        .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
        .width = gpu->width,
        .height = gpu->height
    };

    memset(res, 0, sizeof(*res));
    if (!virtio_send_command(gpu->vdev, &create_req, sizeof(create_req), res, sizeof(*res))) {
        return false;
    }

    if (res->type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_TRACE, "[virtio-gpu] RESOURCE_CREATE_2D rejected: 0x%x\n", res->type);
        return false;
    }

    // Step 2: Attach Backing Storage using page-table physical translation
    struct virtio_gpu_resource_attach_backing_req attach_req = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
        .resource_id = gpu->resource_id,
        .nr_entries = 1,
        .entry = {
            .addr = fb_phys_addr,
            .length = gpu->width * gpu->height * sizeof(uint32_t),
            .padding = 0
        }
    };

    memset(res, 0, sizeof(*res));
    if (!virtio_send_command(gpu->vdev, &attach_req, sizeof(attach_req), res, sizeof(*res))) {
        return false;
    }

    if (res->type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_TRACE, "[virtio-gpu] RESOURCE_ATTACH_BACKING rejected: 0x%x\n", res->type);
        return false;
    }

    // Step 3: Set Scanout
    struct virtio_gpu_set_scanout scanout_req = {
        .hdr = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r = { .x = 0, .y = 0, .width = gpu->width, .height = gpu->height },
        .scanout_id = 0,
        .resource_id = gpu->resource_id
    };

    memset(res, 0, sizeof(*res));
    if (!virtio_send_command(gpu->vdev, &scanout_req, sizeof(scanout_req), res, sizeof(*res))) {
        return false;
    }

    if (res->type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_TRACE, "[virtio-gpu] SET_SCANOUT rejected: 0x%x\n", res->type);
        return false;
    }

    printk(LOG_TRACE, "[virtio-gpu] VirtIO-GPU initialized successfully!\n");
    return true;
}

void virtio_gpu_flush(virtio_gpu_device_t *gpu, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!gpu || !gpu->vdev || width == 0 || height == 0) return;

    struct virtio_gpu_ctrl_hdr *res = get_response_buffer();
    if (!res) return;

    struct virtio_gpu_transfer_to_host_2d transfer_req = {
        .hdr = { .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D },
        .r = { .x = x, .y = y, .width = width, .height = height },
        .offset = (y * gpu->width + x) * sizeof(uint32_t),
        .resource_id = gpu->resource_id,
        .padding = 0
    };

    memset(res, 0, sizeof(*res));
    virtio_send_command(gpu->vdev, &transfer_req, sizeof(transfer_req), res, sizeof(*res));

    struct virtio_gpu_resource_flush flush_req = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH },
        .r = { .x = x, .y = y, .width = width, .height = height },
        .resource_id = gpu->resource_id,
        .padding = 0
    };

    memset(res, 0, sizeof(*res));
    virtio_send_command(gpu->vdev, &flush_req, sizeof(flush_req), res, sizeof(*res));
}

void virtio_gpu_fill_rect(virtio_gpu_device_t *gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!gpu || !gpu->framebuffer || w == 0 || h == 0) return;

    // Perform pure CPU framebuffer fill
    for (uint32_t cy = y; cy < y + h && cy < gpu->height; cy++) {
        uint32_t row_offset = cy * gpu->width;
        for (uint32_t cx = x; cx < x + w && cx < gpu->width; cx++) {
            gpu->framebuffer[row_offset + cx] = color;
        }
    }

    // Flush whole region at once instead of per-pixel or per-line
    virtio_gpu_flush(gpu, x, y, w, h);
}