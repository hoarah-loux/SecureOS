#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <stddef.h>
#include <stdint.h>

typedef struct block_device block_device_t;

typedef int (*block_device_read_fn)(void *context, uint64_t sector, uint32_t count, uint8_t *buffer);

typedef struct {
    block_device_read_fn read;
} block_device_ops_t;

typedef struct {
    const uint8_t *base;
    uint64_t size_bytes;
    uint32_t sector_size;
    uint64_t sector_count;
} block_memory_backend_t;

struct block_device {
    const char *name;
    uint32_t sector_size;
    uint64_t sector_count;
    void *context;
    const block_device_ops_t *ops;
    block_memory_backend_t memory_backend;
};

int block_device_init_memory(block_device_t *device, const char *name, const void *base, uint64_t size_bytes, uint32_t sector_size);
int block_device_read(block_device_t *device, uint64_t sector, uint32_t count, uint8_t *buffer);

#endif /* BLOCK_DEVICE_H */