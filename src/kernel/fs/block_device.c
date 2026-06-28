#include "fs/block_device.h"

static int block_memory_read(void *context, uint64_t sector, uint32_t count, uint8_t *buffer) {
    block_memory_backend_t *backend = (block_memory_backend_t *)context;

    if (!backend || !buffer) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if (backend->sector_size == 0 || backend->sector_count == 0) {
        return -1;
    }

    if (sector >= backend->sector_count || count > backend->sector_count - sector) {
        return -1;
    }

    uint64_t byte_offset = sector * (uint64_t)backend->sector_size;
    uint64_t byte_count = (uint64_t)count * (uint64_t)backend->sector_size;
    if (byte_offset + byte_count > backend->size_bytes) {
        return -1;
    }

    const uint8_t *src = backend->base + byte_offset;
    for (uint64_t i = 0; i < byte_count; i++) {
        buffer[i] = src[i];
    }

    return 0;
}

static const block_device_ops_t memory_device_ops = {
    .read = block_memory_read,
};

int block_device_init_memory(block_device_t *device, const char *name, const void *base, uint64_t size_bytes, uint32_t sector_size) {
    if (!device || !base || sector_size == 0) {
        return -1;
    }

    uint64_t sector_count = size_bytes / (uint64_t)sector_size;
    if (sector_count == 0) {
        return -1;
    }

    device->memory_backend.base = (const uint8_t *)base;
    device->memory_backend.size_bytes = size_bytes;
    device->memory_backend.sector_size = sector_size;
    device->memory_backend.sector_count = sector_count;

    device->name = name;
    device->sector_size = sector_size;
    device->sector_count = sector_count;
    device->context = &device->memory_backend;
    device->ops = &memory_device_ops;

    return 0;
}

int block_device_read(block_device_t *device, uint64_t sector, uint32_t count, uint8_t *buffer) {
    if (!device || !device->ops || !device->ops->read) {
        return -1;
    }

    return device->ops->read(device->context, sector, count, buffer);
}