#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <t.h>
#include <queues.h>
#include <context.h>
#include <heap.h>

#define MAX_CPUS 1
#define SCHEDULER_DEFAULT_QUANTUM_TICKS 5

typedef struct scheduler_cpu {
    uint64_t cpu_id;
    thread *current_thread;
    thread_queue *ready_queue;
    thread_queue *sleep_queue;
    thread_queue *dying_queue;
    thread_queue *keyboard_wait_queue;
    thread *idle_thread;
    uint64_t tick_count;
    uint64_t default_quantum_ticks;
    bool present;
    bool is_initialized;
    bool need_resched;
} scheduler_cpu;

typedef struct scheduler_init_out {
    bool success;
    uint64_t error_code;
    uint64_t initialized_cpu_count;
} scheduler_init_out;

typedef struct scheduler_cpu_init_out {
    bool success;
    uint64_t error_code;
    uint64_t cpu_id;
    scheduler_cpu *cpu;
} scheduler_cpu_init_out;

enum scheduler_error {
    SCHED_INVALID_CPU_ID,
    SCHED_READY_QUEUE_INIT_FAIL,
    SCHED_SLEEP_QUEUE_INIT_FAIL,
    SCHED_DYING_QUEUE_INIT_FAIL,
    SCHED_KEYBOARD_WAIT_QUEUE_INIT_FAIL,
};

enum sched_add_ready_err{
    INVALID_THREAD,
    SCHED_NOT_READY,
    THREAD_ALREADY_IN_OTHER_Q,

};

typedef struct sched_add_ready_out{

    bool success;
    uint64_t error_code;

}sched_add_ready_out;

typedef struct scheduler_yield_out{

    bool success;
    uint64_t error_code;
    thread *yielded_t;

}scheduler_yield_out;


scheduler_init_out scheduler_init(void); // initializes the scheduler system and CPU0 scheduler state
scheduler_cpu_init_out scheduler_cpu_init(uint64_t cpu_id); // initializes one CPU's scheduler queues and counters

scheduler_cpu *scheduler_get_cpu(uint64_t cpu_id); // returns the scheduler state for a specific CPU id
scheduler_cpu *scheduler_this_cpu(void); // returns the scheduler state for the currently running CPU

thread *scheduler_current(uint64_t cpu_id); // returns the current running thread on this CPU
uint64_t scheduler_set_current(uint64_t cpu_id ,thread *t); // sets the current running thread on this CPU

uint64_t scheduler_set_idle_thread(uint64_t cpu_id, thread *t); // registers the CPU's idle thread, used as the pick_next fallback

sched_add_ready_out scheduler_add_ready(uint64_t cpu_id, thread *t); // marks a thread READY and enqueues it for execution
thread *scheduler_pick_next(uint64_t cpu_id); // picks and removes the next runnable thread from the ready queue

scheduler_yield_out scheduler_yield(uint64_t cpu_id); // handles voluntary reschedule from the yield interrupt path
uint64_t scheduler_tick(uint64_t cpu_id); // records a timer tick and requests preemption when a time slice expires

uint64_t scheduler_block_current(uint64_t cpu_id, thread_queue *wait_queue); // blocks the current thread on a subsystem wait queue
uint64_t scheduler_wake_one(uint64_t cpu_id, thread_queue *wait_queue); // wakes one blocked thread from a wait queue and makes it READY
uint64_t scheduler_wake_all(uint64_t cpu_id, thread_queue *wait_queue); // wakes all blocked threads from a wait queue and makes them READY

uint64_t scheduler_exit_current(uint64_t cpu_id);