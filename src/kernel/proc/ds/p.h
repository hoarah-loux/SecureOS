#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mm/alloc/vmm.h"

struct thread;


typedef struct proc{

    uint64_t id;                 // globally unique process identifier (PID)
    uint64_t parent_pid;         // cached parent PID for quick lookup/debug
    struct proc *next;           // next process in global/all-process list
    struct proc *prev;           // previous process in global/all-process list
    vm_space* adress_space;      // owning virtual address space of this process
    struct thread *main_thread;  // primary thread (entry thread) of the process
    struct thread *thread_list;  // legacy alias/head pointer for thread linkage
    struct thread *thread_head;  // first thread in this process thread list
    struct thread *thread_tail;  // last thread in this process thread list
    uint64_t state;              // lifecycle state (enum p_states)
    uint32_t flags;              // process attribute flags (enum p_flags)
    uint32_t refcount;           // references held on this process object
    volatile uint32_t lock;      // lightweight lock placeholder for proc fields
    uint64_t thread_count;       // number of threads currently attached
    uint64_t exit_status;        // exit code/status visible to parent/reaper
   
    

    //this is for later shit
    struct proc *child_head;     // first child process in child list
    struct proc *child_tail;     // last child process in child list
    struct proc *child_list;     // legacy alias/head pointer for children
    struct proc *sibling_prev;   // previous sibling in parent's child list
    struct proc *sibling_next;   // next sibling in parent's child list
    struct proc *parent;         // direct parent process pointer


    
    
    
}proc;



typedef struct create_proc_out{

    proc *proc;
    bool success;
    uint64_t error_code;

}create_proc_out;


typedef struct destroy_proc_out{

    bool success;
    uint64_t error_code;


}destroy_proc_out;

enum p_states{

    NEW,        // process object was created but not runnable yet
    READY,      // process has runnable threads and can be scheduled
    RUNNING,    // process currently has at least one running thread
    WAITING,    // process has no runnable threads and is blocked
    TERMINATED, // process has exited and is awaiting final cleanup/reap

};

enum p_flags{

    PROC_F_KERNEL   = 1u << 0, // kernel process (no user-mode execution)
    PROC_F_SYSTEM   = 1u << 1, // core system process/service
    PROC_F_EXITING  = 1u << 2, // process teardown has started
    PROC_F_REAPABLE = 1u << 3, // process is a zombie and can be reaped
    PROC_F_REAPED   = 1u << 4,
    PROC_F_ORPHANED = 1u << 5, // parent is gone and process is orphaned
    PROC_F_TRACED   = 1u << 6, // reserved for debugging/trace control
   
};


enum destroy_proc_error{
    THREAD_ACTIVE,
    PROCESS_IS_REFED,
    PROC_NOT_TERMINATED,
    PROC_STILL_HAS_CHILD,
    

};


create_proc_out create_proc();

destroy_proc_out destroy_proc();


