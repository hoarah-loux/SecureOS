#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vmm.h>
#include <stdbool.h>
#include <t.h>
#include <heap.h>




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


typedef enum trap_reason {
    TRAP_REASON_TIMER,
    TRAP_REASON_YIELD,
    TRAP_REASON_EXIT,
    TRAP_REASON_SYSCALL,
    TRAP_REASON_FAULT,
} trap_reason;

typedef struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags; 
    uint64_t rsp, ss; // ss here is the same as ds, but iretq treats it as ss so have to keep it ss
} trap_frame;


typedef struct save_trap_frame_out{

    trap_frame *frame_saved;
    context *context_saved;
    bool success;

}save_trap_frame_out;


save_trap_frame_out save_trap_frame(trap_frame *frame, thread *t);

extern void restore_context(context *c) __attribute__((noreturn));