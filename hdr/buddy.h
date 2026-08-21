#include <stddef.h>
#include <stdint.h>

#define MAX_ORDER 11 // 2^0 = 1 frame (4KiB) to 2^10 = 1024 frames (4 MiB)
#define MAX_BLOCK_SIZE (4096 * (1 << MAX_ORDER)) // 4 MiB
#define MIN_BLOCK_SIZE 4096 // 4 KiB
extern struct frame* frame_metadata;

struct buddy_node {
    struct buddy_node* next;
};

struct frame {
    uint8_t flags; // 0 = free, 1 = used
    uint8_t order;
};

int size_to_order(size_t nbytes);

// frame_metadata is a raw low/physical address (set in pmm.c, right after
// the kernel image) - only directly dereferenceable pre-transition (full
// low 4GiB identity map) or post-transition via phys_to_virt() (transition()
// only preserves a small identity-mapped sliver around the kernel image).
// Always index through this instead of frame_metadata directly.
struct frame* fm_base(void);

void buddy_init(uintptr_t base, size_t size, struct frame* frames);
uintptr_t buddy_alloc_blocks(size_t order);
void buddy_free_blocks(uintptr_t addr, size_t order);