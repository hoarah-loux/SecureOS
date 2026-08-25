#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <p.h>
#include <vmm.h>
#include <context.h>


struct thread_queue;





typedef struct thread{

    uint64_t id;
    bool is_kernel;
    struct proc *owning_proc;
    struct thread *next;
    struct thread *prev;
    uintptr_t user_stack_va_start;
    uintptr_t user_stack_va_end;
    size_t user_stack_size;
    uintptr_t kernel_stack_va_start;
    uintptr_t kernel_stack_va_end;
    uintptr_t _rsp_k; //this starts at the va end and goes to the va start bcz its a stack it grows down
    uintptr_t _rsp_u;
    size_t kernel_stack_size;
    struct context *context;
    struct thread *q_thread_next;
    struct thread *q_thread_prev;
    uint64_t flags;
    uint64_t state;
    uint64_t ref_count;
    uint64_t exit_status;
    vm_region *ker_stack_region_ptr;//pointers to regions to free mem
    vm_region *usr_stack_region_ptr;
    vm_region *t_region_list; //thread region list
    vm_region *t_k_a_region_list; //kernel asset thread region list
    struct thread_queue *owner_queue;

    // x87/SSE register file, saved and restored around every context switch.
    //
    // The kernel itself is built -mno-sse and never touches these registers,
    // so this exists purely so ring3 can. It is not optional there: the
    // x86-64 SysV ABI passes floating-point arguments in xmm0-7, sets al to
    // the vector-register count for varargs, and gcc emits SSE for ordinary
    // struct and string copies at any optimisation level. Without a per-thread
    // save area two user threads would silently corrupt each other's
    // floating-point state.
    //
    // Saving is eager: fxsave in save_trap_frame(), fxrstor in
    // restore_context. Lazy switching via CR0.TS and #NM would avoid the cost
    // for threads that never use SSE, but it needs a "who owns the FPU"
    // tracker and a #NM handler, and this kernel does not have enough
    // FPU-using threads for that to pay for itself.
    //
    // FXSAVE requires a 16-byte-aligned destination and faults otherwise.
    // thread objects come from the slab allocator, which makes no alignment
    // promise beyond the natural one, so the area is over-allocated by 16
    // bytes and fpu_state points at the first aligned address inside it.
    // Everything else reads fpu_state and never the raw array.
    //
    // Appended at the end of the struct on purpose: context_switch.asm reads
    // fields by hardcoded offset (THREAD_RSP_K), so inserting anywhere earlier
    // would move them. See the _Static_asserts in src/kernel/proc/ds/t.c.
    uint8_t *fpu_state;
    uint8_t fpu_state_raw[512 + 16];

    // Timer tick at which a sleeping thread becomes runnable again, or 0 when
    // the thread is not sleeping. Set by SYS_sleep, consumed by timer_handler.
    //
    // It lives on the thread rather than in the sleep queue because SYS_sleep
    // is re-entrant by design: syscall_handler rewinds RIP past the `int 0x80`
    // before blocking, so a woken thread re-executes the whole syscall. Left
    // to recompute its deadline from scratch each time, a sleeping thread
    // would never wake. A non-zero wake_tick is how the second execution knows
    // it is a continuation rather than a new request.
    //
    // Appended after fpu_state for the same reason fpu_state was appended at
    // the end: context_switch.asm reads this struct by hardcoded offset.
    uint64_t wake_tick;

}thread;

// FXSAVE/FXRSTOR area size. The layout is fixed by the architecture; only the
// size and the two fields initialised at thread creation matter here.
#define FPU_STATE_SIZE 512

// Offsets within the FXSAVE area of the two control words that must not start
// as zero. FCW 0x037F is the x87 default (round to nearest, all exceptions
// masked, 64-bit precision); MXCSR 0x1F80 is the SSE default (all exceptions
// masked, no flush-to-zero).
#define FPU_STATE_FCW_OFFSET   0
#define FPU_STATE_MXCSR_OFFSET 24
#define FPU_STATE_FCW_INIT     0x037Fu
#define FPU_STATE_MXCSR_INIT   0x1F80u

typedef struct create_thread_out{


    thread *thread;
    bool success;
    uint64_t error_code;


}create_thread_out;


enum create_thread_error{
    USER_STACK_RESERVE_ERROR,
    KERNEL_STACK_RESERVE_ERROR,
    CONTEXT_ALLOC_FAIL,

};

enum thread_flags{

    THREAD_F_START_SUSPENDED = 1u << 0 ,
    THREAD_F_MAIN_THREAD = 1u << 1,
    THREAD_F_REAPED = 1u << 2,
    THREAD_F_REAPABLE = 1U << 3,


};

enum t_states{

    NEW_T,        // thread object was created but not runnable yet
    READY_T,      // thread has runnable threads and can be scheduled
    RUNNING_T,    // thread currently has at least one running thread
    BLOCKED_T,    // Thread is waiting
    DYING_T, // thread has exited and is awaiting final cleanup/reap

};


typedef struct thread_exit_out {
    bool success;
    uint64_t error_code;
    thread *exiting_thread;
} thread_exit_out;

enum thread_exit_error {
    THREAD_EXIT_INVALID_THREAD = 1,
    THREAD_EXIT_INVALID_OWNER,
    THREAD_EXIT_INVALID_REFCOUNT,
    THREAD_EXIT_ALREADY_DYING,
    THREAD_EXIT_ALREADY_REAPED,
    THREAD_EXIT_NOT_RUNNING,
    THREAD_EXIT_STILL_QUEUED,
    THREAD_EXIT_CORRUPT_PROCESS_LIST,
    THREAD_EXIT_PROC_EXIT_FAILED,
};


typedef struct reap_thread_out {
    bool success;
    uint64_t error_code;
    thread *reaped_thread;
    uint64_t exit_status;
    bool was_last_thread;
} reap_thread_out;

enum reap_thread_error {
    THREAD_REAP_INVALID_THREAD = 1,
    THREAD_REAP_INVALID_OWNER,
    THREAD_REAP_INVALID_REFCOUNT,
    THREAD_REAP_ALREADY_REAPED,
    THREAD_REAP_NOT_DYING,
    THREAD_REAP_NOT_REAPABLE,
    THREAD_REAP_STILL_QUEUED,
    THREAD_REAP_EMPTY_PROCESS_LIST,
    THREAD_REAP_NOT_IN_PROCESS,
    THREAD_REAP_CORRUPT_PROCESS_LIST,
    THREAD_REAP_PROC_REAP_FAILED,
};


typedef struct destroy_thread_out {
    bool success;
    uint64_t error_code;
    uint64_t destroyed_thread_id;
} destroy_thread_out;

enum destroy_thread_error {
    THREAD_DESTROY_INVALID_THREAD = 1,
    THREAD_DESTROY_INVALID_OWNER,
    THREAD_DESTROY_INVALID_REFCOUNT,
    THREAD_DESTROY_NOT_DYING,
    THREAD_DESTROY_NOT_REAPED,
    THREAD_DESTROY_STILL_REAPABLE,
    THREAD_DESTROY_STILL_QUEUED,
    THREAD_DESTROY_STILL_PROCESS_ATTACHED,
    THREAD_DESTROY_INVALID_ADDRESS_SPACE,
    THREAD_DESTROY_INVALID_KERNEL_STACK_REGION,
    THREAD_DESTROY_INVALID_USER_STACK_REGION,
    THREAD_DESTROY_INVALID_CONTEXT,
    THREAD_DESTROY_CONTEXT_OWNER_MISMATCH,
    THREAD_DESTROY_CONTEXT_FREE_FAILED,
    THREAD_DESTROY_THREAD_FREE_FAILED,
    REGION_DESTROY_FAIL,
};


create_thread_out create_thread(proc *owning_process, uintptr_t rip, uint64_t thread_flags, size_t user_stack_size);

thread_exit_out thread_exit(thread *t, uint64_t exit_status);

reap_thread_out reap_thread(thread *t);

destroy_thread_out destroy_thread(thread *t);
