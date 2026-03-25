#include "pmm.h"

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


void* memset_me(void* ptr, int value, size_t size){

    unsigned char *p = (unsigned char *)ptr;
    unsigned char value = (unsigned char)value;
    unsigned char *end = p + size;

    while(p < end){
        *p = value;
         p++;

    }

    //for(size_t i = 0; i < size; i++){

        //p[i]= value;
    //}

    //return ptr;
   
}


// Returns the physical address of the first free frame, or 0 if none are free
static uintptr_t get_frame(uint8_t *bitmap, size_t bitmap_size) {
    for (size_t i = 0; i < bitmap_size; i++) {
        uint8_t frame_chunk = bitmap[i];
        for (int j = 0; j < 8; j++) {
            size_t frame = i * 8 + j;
            // Check if this bit is free (0)
            if ((frame_chunk & (1 << j)) == 0) {
                return frame * frame_size;
            }
        }
    }
    // No free frame found
    return 0;
}













void pmm_init(uint32_t mb2_info_ptr, uint8_t _kernel_start, uint8_t _kernel_end){

    //frist lets fill all the bitmap vals to 1(used)
    //to ensure no accedintal freeing happens
    //we use bitmap instead of *bitmap Because WE WANT THE ADRESS OF THE ARRAY, not the value of the fist element
    memset_me(bitmap, 0xff, (size_t)BITMAP_SIZE);


    //set frames in the mb2 mmpa to used
    //parse mb2info struct, and get the memory map tag type6
    const uint8_t *mb2_base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    uint32_t size = *(const uint32_t *)mb2_base;
    const uint8_t *tag = mb2_base + 8;
    const uint8_t *mb2_end = mb2_base + size;

    while (tag < mb2_end) {
        uint32_t tag_type = *(const uint32_t *)tag;
        uint32_t tag_size = *(const uint32_t *)(tag + 4);

        if (tag_type == 0) {
            break;
        }
        if (tag_type == 6) {
            // Memory map tag found
            uint32_t entry_size = *(const uint32_t *)(tag + 8);
            //uint32_t entry_version = *(const uint32_t *)(tag + 12); // not used here
            const uint8_t *entry_ptr = tag + 16;
            const uint8_t *entry_end = tag + tag_size;
            while (entry_ptr < entry_end) {
                uint64_t base_addr = *(const uint64_t *)entry_ptr;
                uint64_t length = *(const uint64_t *)(entry_ptr + 8);
                uint32_t type = *(const uint32_t *)(entry_ptr + 16);
                // uint32_t reserved = *(const uint32_t *)(entry_ptr + 20); // not used
                if (type == 1) { // Usable RAM
                    // Mark frames in this region as free
                    uint64_t region_start = base_addr;
                    uint64_t region_end = base_addr + length;
                    for (uint64_t addr = region_start; addr < region_end; addr += frame_size) {
                        size_t frame = addr / frame_size;
                        if (frame < frame_count) {
                            bitmap[frame / 8] &= ~(1 << (frame % 8));
                        }
                    }
                }
                entry_ptr += entry_size;
            }
            // Only one memory map tag, so break after processing
            break;
        }
        tag += (tag_size + 7) & ~7;
    }




    ///we still need to reseve frames for the bootloader and the kernel
    //first we calculate strart and end indices of the frames for the labels kernel start and end

    uintptr_t kernel_start = (uintptr_t)_kernel_start;
    uintptr_t kernel_end = (uintptr_t)_kernel_end;
    uintptr_t start_frame = kernel_start / frame_size;
    uintptr_t end_frame = (kernel_end + frame_size - 1) / frame_size;
    for (uintptr_t frame = start_frame; frame < end_frame; frame++) {
        if (frame < frame_count) {
            bitmap[frame / 8] |= (1 << (frame % 8));
        }
    }


    //now to set frames for the bitmap itself as used for now this is redyundeabnt
    
    uintptr_t bitmap_adress = (uintptr_t)&bitmap;
    uintptr_t bitmap_size_in_bytes = BITMAP_SIZE;
    uintptr_t bitmap_end = bitmap_adress + bitmap_size_in_bytes;
    uintptr_t bit_map_end_frame = (bitmap_end + frame_size - 1) / frame_size;
    uintptr_t bit_map_start_frame = bitmap_adress/frame_size;

    for(uintptr_t frame = bit_map_start_frame; frame < bit_map_end_frame; frame++){

        if(frame<frame_count){

            bitmap[frame/8]|=(1<<(frame % 8));
        }




    }






}


//get adress of a frame
uintptr_t pmm_alloc_frame(void){
    uintptr_t free_frame = get_frame(bitmap, BITMAP_SIZE);
    if (free_frame == 0) {
        return 0; // No free frame available
    }
    size_t frame = free_frame / frame_size;
    bitmap[frame / 8] |= (1 << (frame % 8));
    return free_frame;
}

uintptr_t pmm_alloc_frame_range(uintptr_t start_addr, uintptr_t end_addr) {
    if (end_addr < start_addr) {
        return 0; // Invalid range
    }
    size_t start_frame_index = start_addr / frame_size;
    size_t end_frame_index = (end_addr + frame_size - 1) / frame_size;
    for (size_t frame = start_frame_index; frame < end_frame_index; frame++) {
        bitmap[frame / 8] |= (1 << (frame % 8));
    }
    return start_addr;
}


uintptr_t pmm_free_frame(uintptr_t address_to_free) {
    size_t frame_to_free = address_to_free / frame_size;
    if (frame_to_free >= frame_count) {
        return 0; // Invalid frame
    }
    bitmap[frame_to_free / 8] &= ~(1 << (frame_to_free % 8));
    return 1; // Success
}


uintptr_t pmm_free_range(uintptr_t start_addr, uintptr_t end_addr){

    if(end_addr < start_addr){

        return 0;
    }

    size_t start_frame_index = start_addr/frame_size;
    size_t end_frame_index = (end_addr + frame_size - 1) / frame_size;


    for(uintptr_t frame = start_frame_index; frame < end_frame_index; frame++){//might have to make it <= instead of <

            bitmap[frame / 8] &= ~(1 << (frame % 8));
    }
    return start_addr;

}




uint64_t pmm_free_frame_count(size_t bitmap_size){

    size_t count = 0;
    for (size_t i = 0; i < bitmap_size; i++) {
        uint8_t frame_chunk = bitmap[i];
        for (int j = 0; j < 8; j++) {
            size_t frame = i * 8 + j;
            // Only count real frames, not padding bits
            if (frame < frame_count && (frame_chunk & (1 << j)) == 0) {
                count++;
            }
        }
    }
    return count;


}

uint64_t pmm_used_frame_count(size_t bitmap_size) {
    size_t count = 0;
    for (size_t i = 0; i < bitmap_size; i++) {
        uint8_t frame_chunk = bitmap[i];
        for (int j = 0; j < 8; j++) {
            size_t frame = i * 8 + j;
            if (frame < frame_count && (frame_chunk & (1 << j))) {
                count++;
            }
        }
    }
    return count;
}

//this fucntion is useless for now but it will be needed when switching to dynamic allocation
uint64_t pmm_total_frame_count(size_t bitmap_size) {
    size_t count = 0;
    for (size_t byte_index = 0; byte_index < bitmap_size; byte_index++) {
        for (size_t bit_index = 0; bit_index < 8; bit_index++) {
            //this account for if the bit isnt mapping an real frames (if the bit represents padding) 
            size_t frame = byte_index * 8 + bit_index;
            if (frame < frame_count) {
                count++;
            }
        }
    }
    return count;
}

//implemnt later
uint64_t max_phys(){

}