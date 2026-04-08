#include "buddy.h"


// list of free blocks
static struct buddy_node* free_lists[MAX_ORDER];

// List of all frames, indexed by frame number
struct frame* frame_metadata = NULL; // we set this shit in pmm.c to the actual mem address (very cursed)

uintptr_t mem_base_addr; // The start of the managed physical memory



int size_to_order(size_t nbytes) {
    int order = 0;
    size_t block_size = MIN_BLOCK_SIZE; // Start with 4 KiB
    while (block_size < nbytes && order < MAX_ORDER) {
        block_size *= 2;
        order++;
    }
    return order;
}

void buddy_init(uintptr_t base, size_t size, struct frame* frames) {
    // Initialize buddy allocator structures here
    mem_base_addr = base;
    frame_metadata = frames;
    // Initialize all frames as free
    for (size_t i = 0; i < size / MIN_BLOCK_SIZE; i++) {
        frame_metadata[i].flags = 0; // 0 = free
        frame_metadata[i].order = 0;
    }

}

uintptr_t buddy_alloc_blocks(size_t order) {

    /*
    blab: Check if the order is valid (0 to MAX_ORDER)
    if so, check the next frame in the list of frames
    */

    if (order >= MAX_ORDER) return 0; // invalid order
    
    // if block of the requested order is available, allocate it and return its address
    /*
    Example: if we want to allocate a block of order 2 (16 KiB),
    we check if there is a free block of order 2 in the free list.
    If there is, we remove it from the free list and return its address.
    If there isn't, we need to split a larger block (order 3 or higher)
    into smaller blocks until we get a block of the requested order.
    
    */

    // --- BASE CASE ---
    if (free_lists[order] != NULL) {
        struct buddy_node* block = free_lists[order];
        free_lists[order] = block->next; // Remove block from free list

        // Mark as used
        // FIX: Cast to uintptr_t so subtraction gives raw bytes, then divide by 4KiB
        size_t block_index = ((uintptr_t)block - mem_base_addr) / MIN_BLOCK_SIZE;
        frame_metadata[block_index].flags = 1;
        frame_metadata[block_index].order = order;

        return (uintptr_t)block; // Return the address of the allocated block
    }

    // --- RECURSIVE CASE ---
    // If no block of the requested order is available, try to split a larger block
    uintptr_t higher_block = buddy_alloc_blocks(order + 1);

    // If we successfully allocated a larger block, split it into two blocks of the requested order
    if (higher_block != 0) {

        // SPLIT THE FUCKING BLOCK HERE
        uintptr_t buddy_addr = higher_block + (1UL << (order + 12)); // Calculate buddy address (order + 12 because block size is 2^order * 4 KiB)

        // Add the buddy block to the free list of the requested order
        struct buddy_node* buddy_block = (struct buddy_node*)buddy_addr;
        buddy_block->next = free_lists[order];
        free_lists[order] = buddy_block;

        // Keep track of metadata for buddy
        // FIX: The buddy is FREE, so we mark it as 0. 
        size_t buddy_index = (buddy_addr - mem_base_addr) / MIN_BLOCK_SIZE;
        frame_metadata[buddy_index].flags = 0; // The buddy block is free
        frame_metadata[buddy_index].order = order; 

        // Update metadata for the block we are returning
        size_t block_index = (higher_block - mem_base_addr) / MIN_BLOCK_SIZE; 
        // Note: flags is already 1 from the base case. We just update the order.
        frame_metadata[block_index].order = order; 

        return higher_block; // Return the address of the allocated block
    }

    return 0; // No block available
    // Literally out of memory at this point
}

static uintptr_t get_buddy_addr(uintptr_t addr, size_t order) {
    size_t block_size = (1UL << (order + 12)); // 2^order * 4096
    uintptr_t relative_addr = addr - mem_base_addr;
    uintptr_t buddy_relative_addr = relative_addr ^ block_size;
    return mem_base_addr + buddy_relative_addr;
}

void buddy_free_blocks(uintptr_t addr, size_t order) {
    if (order >= MAX_ORDER - 1) {
        // Add to the highest possible free list and stop
        struct buddy_node* node = (struct buddy_node*)addr;
        node->next = free_lists[order];
        free_lists[order] = node;
        return;
    }

    uintptr_t buddy_addr = get_buddy_addr(addr, order);
    size_t buddy_index = (buddy_addr - mem_base_addr) / MIN_BLOCK_SIZE;

    // Check if the buddy is free AND of the same order
    // A block can only merge if its buddy is also free and not part of a larger allocation
    if (frame_metadata[buddy_index].flags == 0 && frame_metadata[buddy_index].order == order) {
        
        // --- COALESCING ---
        // 1. Remove buddy from its current free list
        struct buddy_node** curr = &free_lists[order];
        while (*curr != NULL) {
            if ((uintptr_t)*curr == buddy_addr) {
                *curr = (*curr)->next;
                break;
            }
            curr = &((*curr)->next);
        }

        // 2. Determine the start of the combined block (the lower address)
        uintptr_t merged_addr = (addr < buddy_addr) ? addr : buddy_addr;

        // 3. Recursively free the larger merged block
        buddy_free_blocks(merged_addr, order + 1);
        
    } else {
        // --- NO MERGING ---
        // Just add the current block back to the free list
        frame_metadata[(addr - mem_base_addr) / MIN_BLOCK_SIZE].flags = 0;
        frame_metadata[(addr - mem_base_addr) / MIN_BLOCK_SIZE].order = order;

        struct buddy_node* node = (struct buddy_node*)addr;
        node->next = free_lists[order];
        free_lists[order] = node;
    }
}