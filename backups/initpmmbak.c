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