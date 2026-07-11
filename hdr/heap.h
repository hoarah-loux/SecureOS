#pragma once

#include <stddef.h>
#include <stdint.h>

// Linker-defined bounds for the early kernel heap.
extern uint8_t __kheap_start[];
extern uint8_t __kheap_end[];

// Initialize the heap allocator using the linker-provided heap range.
void kheap_init(void);

// Allocate a block of at least `size` bytes from the kernel heap.
void* kmalloc(size_t size);

// Allocate `count * size` bytes and zero-initialize the block.
void* kcalloc(size_t count, size_t size);

// Resize an existing heap allocation, preserving contents up to new_size.
void* krealloc(void* ptr, size_t new_size);

// Free a previously allocated heap block.
uint64_t kfree(void* ptr);

// Optional heap stats helpers for debugging.
size_t kheap_free_bytes(void);
size_t kheap_used_bytes(void);
