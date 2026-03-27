// Parse Multiboot2 tags to find the memory map tag (type 6)
    const uint8_t *mb2_base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    uint32_t total_size = *(const uint32_t *)mb2_base;
    //*tag_ptr will be better named *tag_adress
    const uint8_t *tag_ptr = mb2_base + 8;
    const uint8_t *mb2_end = mb2_base + total_size;

    while (tag_ptr < mb2_end) {
        uint32_t tag_type = *(const uint32_t *)tag_ptr;
        uint32_t tag_size = *(const uint32_t *)(tag_ptr + 4);
        if (tag_type == 0) {
            // End tag
            break;
        }
        if (tag_type == 6) {
            // Found memory map tag; process it later
            break;
        }
        // Advance to next tag (8-byte aligned)
        tag_ptr += (tag_size + 7) & ~7;
    }








//he real bk

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