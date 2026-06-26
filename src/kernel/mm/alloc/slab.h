#pragma once

#include <stddef.h>
#include <stdint.h>

// Slab allocator API built on top of a fixed heap region.
// This allocates fixed-size objects from size-class caches.

// Initialize the slab allocator with a heap-backed region.
uint64_t slab_init(uintptr_t *heap_start, size_t heap_size);

// Allocate an object of at least `size` bytes.
void *slab_alloc(size_t size);

// Free an object previously returned by slab_alloc.
uint64_t slab_free(void *ptr);

// Optional stats helpers for debugging.
size_t slab_free_bytes(void);
size_t slab_used_bytes(void);
