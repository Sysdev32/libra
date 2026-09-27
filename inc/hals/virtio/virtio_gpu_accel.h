#ifndef VIRTIO_GPU_ACCEL_H
#define VIRTIO_GPU_ACCEL_H

#include <stdint.h>
#include <stdbool.h>
#include "virtio_gpu.h"

#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1
#define VIRTIO_GPU_F_RESOURCE_UUID  2
#define VIRTIO_GPU_F_RESOURCE_BLOB  3
#define VIRTIO_GPU_F_CONTEXT_INIT   4

#define VIRTIO_GPU_CMD_CTX_CREATE          0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY         0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D           0x0207

#define VIRTIO_GPU_RESP_OK_CAPSET_INFO     0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET          0x1103
#define VIRTIO_GPU_RESP_ERR_UNSPEC         0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY  0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

typedef struct {
    virtio_gpu_device_t *gpu;
    bool enabled;
    bool virgl;
    bool resource_blob;
    bool context_init;
    uint32_t next_ctx_id;
    uint32_t next_resource_id;
} virtio_gpu_accel_t;

struct virtio_gpu_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} __attribute__((packed));

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

bool virtio_gpu_accel_init(virtio_gpu_accel_t *accel, virtio_gpu_device_t *gpu);
bool virtio_gpu_accel_create_context(virtio_gpu_accel_t *accel, uint32_t ctx_id, const char *name);
bool virtio_gpu_accel_destroy_context(virtio_gpu_accel_t *accel, uint32_t ctx_id);
uint32_t virtio_gpu_accel_create_3d_resource(virtio_gpu_accel_t *accel,
                                             uint32_t target,
                                             uint32_t format,
                                             uint32_t bind,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t depth);
bool virtio_gpu_accel_attach_resource(virtio_gpu_accel_t *accel, uint32_t ctx_id, uint32_t resource_id);
bool virtio_gpu_accel_detach_resource(virtio_gpu_accel_t *accel, uint32_t ctx_id, uint32_t resource_id);
bool virtio_gpu_accel_submit_3d(virtio_gpu_accel_t *accel, uint32_t ctx_id, const void *commands, uint32_t size);
bool virtio_gpu_accel_transfer_to_host_3d(virtio_gpu_accel_t *accel,
                                          uint32_t ctx_id,
                                          uint32_t resource_id,
                                          const struct virtio_gpu_box *box,
                                          uint64_t offset,
                                          uint32_t stride,
                                          uint32_t layer_stride);

#endif
