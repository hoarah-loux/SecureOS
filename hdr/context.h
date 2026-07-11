#pragma once
#include <stddef.h>
#include <stdint.h>


struct thread;


typedef struct context{

    uintptr_t rip;
    uintptr_t rsp;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rbx;
    //all generals regs
    uintptr_t cr3;
    struct thread *owner_thread;


}context;