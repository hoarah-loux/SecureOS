#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vmm.h>
#include <stdbool.h>


struct thread;


typedef struct context {

    bool k;
    // full GPR set (safe for context switch)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    // kernel scheduling resume state
    uintptr_t rip;
    uintptr_t kernel_rsp;
    uint64_t  rflags;
    uintptr_t cr3;
    uintptr_t user_rsp;
    uintptr_t cs;
    uintptr_t ss;
    struct thread *owner_thread;

} context;