#include <multiboot2_utils.h>

const struct multiboot_tag *mb2_find_tag(uint32_t mb2_info_ptr, uint32_t want_type) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    if (!base) {
        return 0;
    }
    uint32_t total_size = *(const uint32_t *)(const void *)base;
    if (total_size < 8) {
        return 0;
    }
    const uint8_t *end = base + total_size;
    if (end < base) {
        return 0;
    }
    const uint8_t *p = base + 8; /* skip total_size + reserved */

    while (p + sizeof(struct multiboot_tag) <= end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(const void *)p;
        if (tag->type == 0) {
            break;
        }
        if (tag->type == want_type) {
            return tag;
        }
        /* tags are 8-byte aligned */
        if (tag->size < sizeof(struct multiboot_tag)) {
            break;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next == 0 || p + next > end) {
            break;
        }
        p += next;
    }
    return 0;
}
