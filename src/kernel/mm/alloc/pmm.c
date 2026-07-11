#include <pmm.h>
#include <buddy.h>
#include <serial.h>
#include <hexdump.h>

#include <string.h>

//constants

// PMM bitmap constants
#define frame_size   (4096UL)
#define max_pyhs     (0x100000000UL) //4gib
#define frame_count  ((max_pyhs) / (frame_size))
#define BITMAP_SIZE  ((frame_count) / 8)
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];


//WE NEED TO COPY THE ENTIRITY OF LOW MEM CONTAINING THE MB2 INFO STRUCT AND OTEHR STUFF FROM EARLY BOOT TO LOCAL KERNEL
//DATA STRUCTURES, or maybe just map it im not sure

// here we have 8 bits per entry
// our frame is represented by one bit only
// so we will have to encode the 1 or 0 in the entry itself
static uint8_t bitmap[BITMAP_SIZE];
static uint64_t pmm_total_mmap_bytes = 0;
static uint64_t pmm_max_phys_end = 0;
static size_t pmm_managed_frame_count = frame_count;
static size_t pmm_managed_bitmap_size = BITMAP_SIZE;

uintptr_t _pmm_alloc_frame_range(uintptr_t start_addr, uintptr_t end_addr);

void* memset_me(void* ptr, int value, size_t size){

    unsigned char *p = (unsigned char *)ptr;
    unsigned char fill = (unsigned char)value;
    unsigned char *end = p + size;

    while(p < end){
        *p = fill;
         p++;

    }

    //for(size_t i = 0; i < size; i++){

        //p[i]= value;
    //}

    return ptr;
   
}


// Returns the physical address of the first free frame, or 0 if none are free
static uintptr_t get_frame(uint8_t *bitmap, size_t bitmap_size) {
    for (size_t i = 0; i < bitmap_size; i++) {
        uint8_t frame_chunk = bitmap[i];
        for (int j = 0; j < 8; j++) {
            size_t frame = i * 8 + j;
            // Check if this bit is free (0)
            if (frame < pmm_managed_frame_count && (frame_chunk & (1 << j)) == 0) {
                return frame * frame_size;
            }
        }
    }
    // No free frame found
    return 0;
}





void pmm_init(uint32_t mb2_info_ptr, uintptr_t kernel_start, uintptr_t kernel_end) {

    // Start with all frames as used; we'll clear usable frames after map parsing.
    memset_me(bitmap, 0xff, (size_t)BITMAP_SIZE);

    // Parse mb2info struct, and get the memory map tag (type 6)
    const uint8_t *mb2_base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    uint32_t mb2_total_size = *(const uint32_t *)mb2_base; // Fix: Get total size to protect it later
    const uint8_t *tag = mb2_base + 8;
    const uint8_t *mb2_end = mb2_base + mb2_total_size;

    pmm_total_mmap_bytes = 0;
    pmm_max_phys_end = 0;

    while (tag < mb2_end) {
        uint32_t tag_type = *(const uint32_t *)tag;
        uint32_t tag_size = *(const uint32_t *)(tag + 4);

        if (tag_type == 0) break;
        
        if (tag_type == 6) {
            // First pass: collect max phys + total map bytes across all types.
            uint32_t entry_size = *(const uint32_t *)(tag + 8);
            const uint8_t *entry_ptr = tag + 16;
            const uint8_t *entry_end = tag + tag_size;

            while (entry_ptr + entry_size <= entry_end) {
                uint64_t base_addr = *(const uint64_t *)entry_ptr;
                uint64_t length = *(const uint64_t *)(entry_ptr + 8);

                if (UINT64_MAX - pmm_total_mmap_bytes < length) {
                    pmm_total_mmap_bytes = UINT64_MAX;
                } else {
                    pmm_total_mmap_bytes += length;
                }

                uint64_t region_end_all_types = base_addr + length;
                if (region_end_all_types > pmm_max_phys_end) {
                    pmm_max_phys_end = region_end_all_types;
                }
                entry_ptr += entry_size;
            }

            // Set runtime managed size, clamped to our static 4GiB store.
            uint64_t managed_phys_end = (pmm_max_phys_end > max_pyhs) ? max_pyhs : pmm_max_phys_end;

            if (managed_phys_end == 0) {
                pmm_managed_frame_count = 0;
                pmm_managed_bitmap_size = 0;
            } else {
                pmm_managed_frame_count = (size_t)((managed_phys_end + frame_size - 1) / frame_size);
                pmm_managed_bitmap_size = (pmm_managed_frame_count + 7) / 8;
            }

            memset_me(bitmap, 0xff, pmm_managed_bitmap_size);

            // Second pass: mark usable RAM (type 1) as free.
            entry_ptr = tag + 16;
            while (entry_ptr + entry_size <= entry_end) {
                uint64_t base_addr = *(const uint64_t *)entry_ptr;
                uint64_t length = *(const uint64_t *)(entry_ptr + 8);
                uint32_t type = *(const uint32_t *)(entry_ptr + 16);

                if (type == 1) {
                    uint64_t region_start = base_addr;
                    uint64_t region_end = base_addr + length;
                    if (region_end > managed_phys_end) region_end = managed_phys_end;

                    for (uint64_t addr = region_start; addr < region_end; addr += frame_size) {
                        size_t frame = addr / frame_size;
                        if (frame < pmm_managed_frame_count) {
                            bitmap[frame / 8] &= ~(1 << (frame % 8));
                        }
                    }
                }
                entry_ptr += entry_size;
            }
            break;
        }
        tag += (tag_size + 7) & ~7;
    }

    // -=- CRITICAL RESERVATIONS -=-

    // 1. Reserve Kernel frames
    _pmm_alloc_frame_range(0x0, kernel_end);

    // 2. Reserve MB2 Info structure (so it's not overwritten by buddy nodes)
    _pmm_alloc_frame_range(mb2_info_ptr, mb2_info_ptr + mb2_total_size);

    // 3. Reserve the Bitmap's own frames (redundant if inside kernel, but safe)
    uintptr_t bitmap_addr = (uintptr_t)&bitmap;
    _pmm_alloc_frame_range(bitmap_addr, bitmap_addr + pmm_managed_bitmap_size);

    // 4. Set up the Buddy Metadata (frame_metadata)
    // We store this 4 KiB after the kernel end.
    // to align use the ALIGN_UP macro
    uintptr_t metadata_addr = ALIGN_UP(kernel_end, frame_size);
    frame_metadata = (struct frame*)metadata_addr;
    uintptr_t metadata_size = pmm_managed_frame_count * sizeof(struct frame);

    // Mark the metadata frames as USED in the bitmap so we don't overwrite them
    _pmm_alloc_frame_range((uintptr_t)frame_metadata, (uintptr_t)frame_metadata + metadata_size);

    // --- BUDDY HANDOVER ---

    // Init the buddy allocator structures
    buddy_init(0, pmm_managed_frame_count * frame_size, frame_metadata);

    // For every frame still marked as FREE (0) in our bootstrap bitmap, 
    // we hand it over to the buddy allocator.
    for (uintptr_t frame = 0; frame < pmm_managed_frame_count; frame++) {
        if ((bitmap[frame / 8] & (1 << (frame % 8))) == 0) {
            uintptr_t frame_addr = frame * frame_size;
            // Free single block (order 0) into the buddy system. 
            // The buddy system will automatically coalesce these into larger blocks.
            buddy_free_blocks(frame_addr, 0); 
        }
    }
}

//get adress of a frame
uintptr_t _pmm_alloc_frame(void){
    uintptr_t free_frame = get_frame(bitmap, pmm_managed_bitmap_size);
    if (free_frame == 0) {
        return 0; // No free frame available
    }
    size_t frame = free_frame / frame_size;
    bitmap[frame / 8] |= (1 << (frame % 8));
    return free_frame;
}

uintptr_t _pmm_alloc_frame_range(uintptr_t start_addr, uintptr_t end_addr) {
    if (end_addr < start_addr) {
        return 0; // Invalid range
    }
    size_t start_frame_index = start_addr / frame_size;
    size_t end_frame_index = (end_addr + frame_size - 1) / frame_size;
    if (start_frame_index >= pmm_managed_frame_count) {
        return 0;
    }
    if (end_frame_index > pmm_managed_frame_count) {
        end_frame_index = pmm_managed_frame_count;
    }
    for (size_t frame = start_frame_index; frame < end_frame_index; frame++) {
        bitmap[frame / 8] |= (1 << (frame % 8));
    }
    return start_addr;
}


uintptr_t _pmm_free_frame(uintptr_t address_to_free) {
    size_t frame_to_free = address_to_free / frame_size;
    if (frame_to_free >= pmm_managed_frame_count) {
        return 0; // Invalid frame
    }
    bitmap[frame_to_free / 8] &= ~(1 << (frame_to_free % 8));
    return 1; // Success
}


uintptr_t _pmm_free_range(uintptr_t start_addr, uintptr_t end_addr){

    if(end_addr < start_addr){

        return 0;
    }

    size_t start_frame_index = start_addr/frame_size;
    size_t end_frame_index = (end_addr + frame_size - 1) / frame_size;
    if (start_frame_index >= pmm_managed_frame_count) {
        return 0;
    }
    if (end_frame_index > pmm_managed_frame_count) {
        end_frame_index = pmm_managed_frame_count;
    }


    for(uintptr_t frame = start_frame_index; frame < end_frame_index; frame++){//might have to make it <= instead of <

            bitmap[frame / 8] &= ~(1 << (frame % 8));
    }
    return start_addr;

}




uint64_t pmm_free_frame_count(void) {
    size_t count = 0;
    
    for (size_t i = 0; i < pmm_managed_frame_count; i++) {
        // In your buddy implementation, we set flags = 0 for free, 1 for used
        if (frame_metadata[i].flags == 0) {
            count++;
        }
    }
    return count;
}

uint64_t pmm_used_frame_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < pmm_managed_frame_count; i++) {
        if (frame_metadata[i].flags == 1) { // 1 = Allocated
            count++;
        }
    }
    return count;
}

//this fucntion is useless for now but it will be needed when switching to dynamic allocation
uint64_t pmm_total_frame_count(size_t bitmap_size) {
    return (uint64_t)pmm_managed_frame_count;
}

uint64_t pmm_max_phys_addr(void){
    return pmm_max_phys_end;
}

uint64_t pmm_total_map_bytes(void){
    return pmm_total_mmap_bytes;
}

void pmm_debug_bitmap_window(size_t start_byte, size_t byte_count) {
    if (pmm_managed_bitmap_size == 0 || pmm_managed_frame_count == 0) {
        serial_write_string("[PMM] bitmap debug: empty managed bitmap\r\n");
        return;
    }

    if (start_byte >= pmm_managed_bitmap_size) {
        serial_printf("[PMM] bitmap debug: start 0x%zx out of range (size=0x%zx)\r\n", start_byte, pmm_managed_bitmap_size);
        return;
    }

    size_t end_byte = start_byte + byte_count;
    if (end_byte < start_byte || end_byte > pmm_managed_bitmap_size) {
        end_byte = pmm_managed_bitmap_size;
    }

    serial_printf("[PMM] bitmap window: bytes [0x%zx..0x%zx)\r\n", start_byte, end_byte);

    for (size_t i = start_byte; i < end_byte; i++) {
        uint8_t byte = bitmap[i];
        size_t first_frame = i * 8;
        size_t last_frame = first_frame + 7;

        if (first_frame >= pmm_managed_frame_count) {
            break;
        }
        if (last_frame >= pmm_managed_frame_count) {
            last_frame = pmm_managed_frame_count - 1;
        }

        unsigned long long first_phys = (unsigned long long)((uintptr_t)first_frame * frame_size);
        unsigned long long last_phys = (unsigned long long)((uintptr_t)last_frame * frame_size);

        serial_printf(
            "[PMM] bm[0x%04zx]=0x%02x frames[%zu..%zu] phys[0x%llx..0x%llx]\r\n",
            i,
            (unsigned)byte,
            first_frame,
            last_frame,
            first_phys,
            last_phys
        );
    }
}

// calculate address from bitmap index and bit position, return 0 if out of bounds
// PRETTY SURE WE DON'T NEED THIS NO MORE BUT I'LL LEAVE IT FOR NOW.. assaf what do you think?
uintptr_t get_frame_address(size_t byte_index, size_t bit_index) {
    size_t frame = byte_index * 8 + bit_index;
    if (frame >= pmm_managed_frame_count) {
        return 0; // Out of bounds
    }
    return frame * frame_size;
}

// Implement buddy functions for vmm

uintptr_t pmm_alloc_frame(void) {
    return buddy_alloc_blocks(0); // Allocate a single block (order 0)
}

uintptr_t pmm_free_frame(uintptr_t address_to_free) {
    if (address_to_free % frame_size != 0) return 0;

    // find out the order
    size_t frame_idx = address_to_free / frame_size;
    size_t order = frame_metadata[frame_idx].order; // should always be 0... I hope

    buddy_free_blocks(address_to_free, order);
    return 1; // Success
}

uintptr_t pmm_alloc_bytes(size_t size) {
    int order = size_to_order(size); 
    return buddy_alloc_blocks(order);
}