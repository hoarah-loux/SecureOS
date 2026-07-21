#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <t.h>
#include <heap.h>


typedef struct thread_queue{

    uint64_t type;
    thread *head;
    thread *tail;
    uint64_t thread_count;
    

}thread_queue;


enum queue_type{
    
    READY,
    SLEEPING,
    DYING,
    KEYBOARD_WAIT,
    DISK_WAIT,
    CHILD_EXIT_WAIT,
    SEM_WAIT,
    MUTEX_WAIT,
    FS_WAIT,


};

typedef struct thread_queue_init_out{

    bool success;
    thread_queue *q;
    

}thread_queue_init_out;


typedef struct thread_queue_pop_out{

    thread *popped_t;
    bool success;

}thread_queue_pop_out;


typedef struct thread_queue_remove_out{

    thread *removed_thread;
    thread_queue *queue_removed_from;
    bool success;


}thread_queue_remove_out;


thread_queue_init_out thread_queue_init(uint64_t type);



uint64_t thread_queue_push(thread_queue *q, thread *t);

thread_queue_pop_out thread_queue_pop(thread_queue *q);

thread_queue_remove_out thread_queue_remove(thread_queue *q, thread *t);