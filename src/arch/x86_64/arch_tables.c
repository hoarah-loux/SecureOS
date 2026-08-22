#include <arch_tables.h>

extern uint8_t gdt64;
extern uint8_t tss64;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uintptr_t base;
} gdtr_ptr;

static uintptr_t rebase_to_high_alias(uintptr_t value, uintptr_t low_start_va, uintptr_t low_end_va, uintptr_t high_base_va) {
    if (value < low_start_va || value >= low_end_va) {
        return value;
    }

    uintptr_t delta = value - low_start_va;
    uintptr_t rebased = high_base_va + delta;
    if (rebased < high_base_va) {
        return 0;
    }

    return rebased;
}

uint64_t arch_rebase_and_reload_gdt_tss(uintptr_t low_start_va, uintptr_t low_end_va, uintptr_t high_base_va) {
    if (low_start_va >= low_end_va) {
        return 0;
    }

    uintptr_t gdt_base = rebase_to_high_alias((uintptr_t)&gdt64, low_start_va, low_end_va, high_base_va);
    uintptr_t tss_base = rebase_to_high_alias((uintptr_t)&tss64, low_start_va, low_end_va, high_base_va);

    if (gdt_base == 0 || tss_base == 0) {
        return 0;
    }

    // Current boot GDT layout places the 64-bit TSS descriptor at selector 0x28.
    const uintptr_t tss_desc_offset = 0x28;
    uint8_t *tss_desc = (uint8_t *)(gdt_base + tss_desc_offset);

    tss_desc[2] = (uint8_t)(tss_base & 0xFF);
    tss_desc[3] = (uint8_t)((tss_base >> 8) & 0xFF);
    tss_desc[4] = (uint8_t)((tss_base >> 16) & 0xFF);
    tss_desc[7] = (uint8_t)((tss_base >> 24) & 0xFF);
    tss_desc[8] = (uint8_t)((tss_base >> 32) & 0xFF);
    tss_desc[9] = (uint8_t)((tss_base >> 40) & 0xFF);
    tss_desc[10] = (uint8_t)((tss_base >> 48) & 0xFF);
    tss_desc[11] = (uint8_t)((tss_base >> 56) & 0xFF);
    // LTR requires an available TSS descriptor type (0x9), not busy (0xB).
    // Boot code already executed one LTR, which marks the descriptor busy.
    tss_desc[5] = 0x89;

    gdtr_ptr gdtr = {0};
    asm volatile("sgdt %0" : "=m"(gdtr) :: "memory");
    gdtr.base = gdt_base;
    asm volatile("lgdt %0" :: "m"(gdtr) : "memory");

    const uint16_t tss_selector = 0x28;
    asm volatile("ltr %w0" :: "r"(tss_selector) : "memory");

    return 1;
}
