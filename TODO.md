# Missing/broken — block usermode

- [ ] **Syscall entry** — none. No int 0x80 gate, no SYSCALL/SYSRET (STAR/LSTAR/SFMASK never written). `TRAP_REASON_SYSCALL` enum exists, unused.
- [ ] **IDT gate DPL wrong for user traps** — `YIELD_VECTOR`/`EXIT_VECTOR` (0x81/0x82) meant for ring3 but DPL=0 in `idt.c:28-32` — ring3 `int` on them faults. No #GP/#UD handler registered either.
- [ ] **Page mapper never called** — `vmm_map_page()` (`vmm.c:1412`) zero callers. `create_vm_space()` builds PML4 but nothing fills it. No user page ever gets mapped.
- [ ] **ELF loader stub only** — `loader.c` reads header, never walks program headers, never copies segments, no user mapping.
- [ ] **No usermode entry trampoline** — `restore_context()` can build CPL3 IRETQ frame but nothing constructs context from loaded ELF and calls it.
- [ ] **Fault handler empty** — `page_fault_handler()` (`handlers.c:17-72`) branches all 8 cases, every body empty comment. Ring3 fault = infinite re-fault, no kill/SIGSEGV/panic.
- [ ] **kmain never boots a process** — `kernel.c` test-parses ELF header only, never calls `scheduler_init`/`create_proc`/`create_thread`. `init_proc.c` empty (0 bytes).
- [ ] **src/userland/ empty** — no user program source at all.
- [ ] **No libc/syscall stubs** — `src/stdlib/` empty. No open/read/write/exit wrappers, no crt0 for user binaries.

## Bottom line

Hardware scaffolding (GDT/TSS/page bit) ready. Everything trap-in, load-program, map-memory, handle-fault, and userland-side is either stub or unwired from `kmain`.

Order: fix IDT DPL → wire `vmm_map_page` into ELF loader → build usermode-entry trampoline → wire fault handler → call from `kmain` → write minimal userland+syscall stubs.
