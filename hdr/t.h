#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <p.h>
#include <vmm.h>


struct context;



typedef struct thread{

    uint64_t id;
    bool is_kernel;
    struct proc *owning_proc;
    struct thread *next;
    struct thread *prev;
    uintptr_t *user_stack_va_start;
    uintptr_t *user_stack_va_end;
    size_t user_stack_size;
    uintptr_t *kernel_stack_va_start;
    uintptr_t *kernel_stack_va_end;
    uintptr_t *_rsp; //this starts at the va end and goes to the va start bcz its a stack it grows down
    size_t kernel_stack_size;
    struct context *context;
    struct thread *rq_thread_next;
    struct thread *rq_thread_prev;
    uint64_t state;



}thread;

typedef struct create_thread_out{


    thread *thread;
    bool success;
    uint64_t error_code;


}create_thread_out;


enum create_thread_error{
    sadas,

};

enum create_thread_flags{

    THREAD_F_START_SUSPENDED = 1u << 0 ,
    THREAD_F_SET_MAIN_THREAD = 1u << 1,



    

};

enum t_states{

    NEW,        // process object was created but not runnable yet
    READY,      // process has runnable threads and can be scheduled
    RUNNING,    // process currently has at least one running thread
    BLOCKED,    // Thread is waiting
    DYING, // process has exited and is awaiting final cleanup/reap

};




create_thread_out create_thread(proc *owning_process, uintptr_t rip, uint64_t thread_flags, size_t user_stack_size);
 