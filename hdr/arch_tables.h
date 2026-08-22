#pragma once

#include <stdint.h>

// Rebase boot-time GDT/TSS pointers from low alias to higher-half alias,
// patch the TSS descriptor base, then reload GDTR and TR.
uint64_t arch_rebase_and_reload_gdt_tss(uintptr_t low_start_va, uintptr_t low_end_va, uintptr_t high_base_va);
