#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <t.h>
#include <queues.h>
#include <context.h>
#include <heap.h>

#define SCHEDULER_MAX_CPUS 1
#define SCHEDULER_DEFAULT_QUANTUM_TICKS 5

typedef struct scheduler_cpu {
    uint64_t cpu_id;
    thread *current_thread;
    thread_queue *ready_queue;
    thread_queue *sleep_queue;
    thread_queue *dying_queue;
    uint64_t tick_count;
    uint64_t default_quantum_ticks;
    bool need_resched;
} scheduler_cpu;

typedef struct scheduler_init_out {
    bool success;
    uint64_t error_code;
} scheduler_init_out;

enum scheduler_error {
    SCHED_INVALID_CPU_ID,
    SCHED_READY_QUEUE_INIT_FAIL,
    SCHED_SLEEP_QUEUE_INIT_FAIL,
    SCHED_DYING_QUEUE_INIT_FAIL,
};

scheduler_init_out scheduler_init(void); // initializes the scheduler system and CPU0 scheduler state
scheduler_init_out scheduler_cpu_init(uint64_t cpu_id); // initializes one CPU's scheduler queues and counters

scheduler_cpu *scheduler_get_cpu(uint64_t cpu_id); // returns the scheduler state for a specific CPU id
scheduler_cpu *scheduler_this_cpu(void); // returns the scheduler state for the currently running CPU

thread *scheduler_current(void); // returns the current running thread on this CPU
uint64_t scheduler_set_current(thread *t); // sets the current running thread on this CPU

uint64_t scheduler_add_ready(thread *t); // marks a thread READY and enqueues it for execution
thread *scheduler_pick_next(void); // picks and removes the next runnable thread from the ready queue

uint64_t scheduler_yield(trap_frame *frame); // handles voluntary reschedule from the yield interrupt path
uint64_t scheduler_tick(trap_frame *frame); // handles timer ticks and requests preemption when a time slice expires

uint64_t scheduler_block_current(thread_queue *wait_queue); // blocks the current thread on a subsystem wait queue
uint64_t scheduler_wake_one(thread_queue *wait_queue); // wakes one blocked thread from a wait queue and makes it READY
uint64_t scheduler_wake_all(thread_queue *wait_queue); // wakes all blocked threads from a wait queue and makes them READY

uint64_t scheduler_mark_dying(thread *t); // marks a thread DYING and moves it to the dying queue for cleanup
