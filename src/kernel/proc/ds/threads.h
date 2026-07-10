#pragma once
#include <stddef.h>
#include <stdint.h>


struct context;
struct proc;


typedef struct thread{

    uint64_t id;
    struct proc *owning_proc;
    struct thread *next;
    uintptr_t *user_stack;
    uintptr_t *kernel_stack;
    struct context *context;
    uint64_t state;



}thread;