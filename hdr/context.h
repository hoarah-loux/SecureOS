#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vmm.h>


struct thread;


typedef struct context {
    // full GPR set (safe for context switch)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    // kernel scheduling resume state
    uintptr_t rip;
    uintptr_t rsp;
    uint64_t  rflags;
    uintptr_t cr3;

    // first user entry frame (for iretq path)
    uintptr_t user_rip;
    uintptr_t user_rsp;
    uint64_t  user_rflags;
    uint64_t  user_cs;
    uint64_t  user_ss;

    struct thread *owner_thread;
} context;