#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vmm.h>
#include <fat32_fs.h>

#define PROC_FD_BASE 3
#define PROC_MAX_OPEN_FILES 16

struct thread;

typedef enum proc_fd{

PROC_FD_NONE,
PROC_FD_STDIN = 0,
PROC_FD_STDOUT = 1,
PROC_FD_STDERR = 2,
PROC_FD_FILE,

}proc_fd_kind;


typedef struct proc_fd_slot_t{

    bool in_use;
    proc_fd_kind kind;
    fat32_file_t file;



}proc_fd_slot_t;


typedef struct proc{

    uint64_t id;                 // globally unique process identifier (PID)
    uint64_t parent_pid;         // cached parent PID for quick lookup/debug
    struct proc *next;           // next process in global/all-process list
    struct proc *prev;           // previous process in global/all-process list
    vm_space* adress_space;      // owning virtual address space of this process
    struct thread *main_thread;  // primary thread (entry thread) of the process
    
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
        
     
    
    struct proc *sibling_next;    // previous sibling in parent's child list 
    struct proc *sibling_prev;     // next sibling in parent's child list
    struct proc *parent;         // direct parent process pointer
    

    proc_fd_slot_t fd_table[PROC_MAX_OPEN_FILES];

    // Program break, backing SYS_brk and therefore newlib's _sbrk() and
    // everything above it - malloc, stdio buffers, strdup.
    //
    // The whole arena is reserved once at process creation as a single
    // anonymous demand-paged region, and brk() only moves a cursor inside it.
    // That is why there is no region churn here and no need for SYS_munmap:
    // reserving costs nothing but a vm_region until a page is actually
    // touched, and growing the break is a bounds check rather than a mapping
    // operation. A program that never calls malloc pays for one struct.
    //
    // brk_current == brk_base means nothing has been handed out yet. All three
    // are zero for a process with no arena, in which case SYS_brk fails.
    uintptr_t brk_base;      // first byte of the reservation
    uintptr_t brk_current;   // current break; brk_base <= brk_current <= brk_limit
    uintptr_t brk_limit;     // one past the last byte of the reservation

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
    READY_P,    // process has runnable threads and can be scheduled
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
    PROC_F_USER     = 1u << 7,
};


enum destroy_proc_error{
    THREAD_ACTIVE,
    PROCESS_IS_REFED,
    PROC_NOT_TERMINATED,
    PROC_STILL_HAS_CHILD,
    DESTROY_SPACE_FAIL,
    

};

typedef struct  proc_exit_out{

    bool success;
    uint64_t error_code;

}proc_exit_out;

enum proc_exit_error{
    
    ALREADY_REAPED,
    INVALID_ARGS,
    

};

typedef struct reap_proc_out{

    bool success;
    uint64_t error_code;

}reap_proc_out;

enum reap_proc_error{

    wesee,

};



create_proc_out create_proc(bool k);

destroy_proc_out destroy_proc(proc *p);

proc_exit_out proc_exit(proc *p);

reap_proc_out reap_proc (proc*);






