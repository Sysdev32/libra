#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <hals/virtio/virtio_gpu_accel.h>
#include <drivers/fb.h>

static bool accel_send_simple(virtio_gpu_accel_t *accel, void *req, uint32_t req_len) {
    struct virtio_gpu_ctrl_hdr res;

    if (!accel || !accel->enabled || !accel->gpu || !accel->gpu->vdev) {
        return false;
    }

    memset(&res, 0, sizeof(res));
    if (!virtio_send_command(accel->gpu->vdev, req, req_len, &res, sizeof(res))) {
        return false;
    }

    if (res.type != VIRTIO_GPU_RESP_OK_NODATA) {
        printk(LOG_WARNING, "[virtio-gpu-accel] command rejected: 0x%x\n", res.type);
        return false;
    }

    return true;
}

bool virtio_gpu_accel_init(virtio_gpu_accel_t *accel, virtio_gpu_device_t *gpu) {
    if (!accel || !gpu || !gpu->vdev) {
        return false;
    }

    memset(accel, 0, sizeof(*accel));
    accel->gpu = gpu;
    accel->virgl = virtio_has_feature(gpu->vdev, VIRTIO_GPU_F_VIRGL);
    accel->resource_blob = virtio_has_feature(gpu->vdev, VIRTIO_GPU_F_RESOURCE_BLOB);
    accel->context_init = virtio_has_feature(gpu->vdev, VIRTIO_GPU_F_CONTEXT_INIT);
    accel->next_ctx_id = 1;
    accel->next_resource_id = gpu->resource_id + 1;
    accel->enabled = accel->virgl;

    if (!accel->enabled) {
        printk(LOG_INFO, "[virtio-gpu-accel] VirGL/3D not offered; using 2D framebuffer path\n");
        return false;
    }

    printk(LOG_INFO, "[virtio-gpu-accel] VirGL enabled (blob=%u context_init=%u)\n",
           accel->resource_blob ? 1 : 0,
           accel->context_init ? 1 : 0);
    return true;
}

bool virtio_gpu_accel_create_context(virtio_gpu_accel_t *accel, uint32_t ctx_id, const char *name) {
    struct virtio_gpu_ctx_create req;

    if (!accel || !accel->enabled || ctx_id == 0) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    req.hdr.ctx_id = ctx_id;

    if (name) {
        size_t len = strlen(name);
        if (len > sizeof(req.debug_name)) {
            len = sizeof(req.debug_name);
        }
        memcpy(req.debug_name, name, len);
        req.nlen = (uint32_t)len;
    }

    return accel_send_simple(accel, &req, sizeof(req));
}

bool virtio_gpu_accel_destroy_context(virtio_gpu_accel_t *accel, uint32_t ctx_id) {
    struct virtio_gpu_ctx_destroy req;

    if (!accel || !accel->enabled || ctx_id == 0) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    req.hdr.ctx_id = ctx_id;
    return accel_send_simple(accel, &req, sizeof(req));
}

uint32_t virtio_gpu_accel_create_3d_resource(virtio_gpu_accel_t *accel,
                                             uint32_t target,
                                             uint32_t format,
                                             uint32_t bind,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t depth) {
    struct virtio_gpu_resource_create_3d req;
    uint32_t resource_id;

    if (!accel || !accel->enabled || width == 0 || height == 0 || depth == 0) {
        return 0;
    }

    resource_id = accel->next_resource_id++;

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req.resource_id = resource_id;
    req.target = target;
    req.format = format;
    req.bind = bind;
    req.width = width;
    req.height = height;
    req.depth = depth;
    req.array_size = 1;
    req.last_level = 0;
    req.nr_samples = 0;

    if (!accel_send_simple(accel, &req, sizeof(req))) {
        return 0;
    }

    return resource_id;
}

bool virtio_gpu_accel_attach_resource(virtio_gpu_accel_t *accel, uint32_t ctx_id, uint32_t resource_id) {
    struct virtio_gpu_ctx_resource req;

    if (!accel || !accel->enabled || ctx_id == 0 || resource_id == 0) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    return accel_send_simple(accel, &req, sizeof(req));
}

bool virtio_gpu_accel_detach_resource(virtio_gpu_accel_t *accel, uint32_t ctx_id, uint32_t resource_id) {
    struct virtio_gpu_ctx_resource req;

    if (!accel || !accel->enabled || ctx_id == 0 || resource_id == 0) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    return accel_send_simple(accel, &req, sizeof(req));
}

bool virtio_gpu_accel_submit_3d(virtio_gpu_accel_t *accel, uint32_t ctx_id, const void *commands, uint32_t size) {
    struct {
        struct virtio_gpu_cmd_submit req;
        uint8_t command_bytes[3072];
    } packet;

    if (!accel || !accel->enabled || ctx_id == 0 || !commands || size == 0 ||
        size > sizeof(packet.command_bytes)) {
        return false;
    }

    memset(&packet, 0, sizeof(packet));
    packet.req.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    packet.req.hdr.ctx_id = ctx_id;
    packet.req.size = size;
    memcpy(packet.command_bytes, commands, size);

    return accel_send_simple(accel, &packet, sizeof(packet.req) + size);
}

bool virtio_gpu_accel_transfer_to_host_3d(virtio_gpu_accel_t *accel,
                                          uint32_t ctx_id,
                                          uint32_t resource_id,
                                          const struct virtio_gpu_box *box,
                                          uint64_t offset,
                                          uint32_t stride,
                                          uint32_t layer_stride) {
    struct virtio_gpu_transfer_host_3d req;

    if (!accel || !accel->enabled || ctx_id == 0 || resource_id == 0 || !box) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    req.hdr.ctx_id = ctx_id;
    req.box = *box;
    req.offset = offset;
    req.resource_id = resource_id;
    req.stride = stride;
    req.layer_stride = layer_stride;

    return accel_send_simple(accel, &req, sizeof(req));
}
