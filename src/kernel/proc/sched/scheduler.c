#include <scheduler.h>

static scheduler_cpu cpus[MAX_CPUS];


// must be called before anything
void populate(){

    for(int i = 0; i < MAX_CPUS; i++){

        cpus[i].cpu_id = i;
        cpus[i].present = true;

    }
}

scheduler_init_out scheduler_init(){

    scheduler_init_out result = {0};
    populate();
    scheduler_cpu_init_out cpu0_init = scheduler_cpu_init(0);
    if(cpu0_init.success == false){return result;}

    result.initialized_cpu_count = 1;
    result.success = true;

    return result;
}

scheduler_cpu_init_out scheduler_cpu_init(uint64_t cpu_id){

    scheduler_cpu_init_out result = {0};

    if(cpu_id >= MAX_CPUS){
        result.error_code = SCHED_INVALID_CPU_ID;
        return result;
    }

    if(cpus[cpu_id].present == false){
        result.error_code = SCHED_INVALID_CPU_ID;
        return result;
    }

    scheduler_cpu *cpu = &cpus[cpu_id];
    thread_queue_init_out ready_q =  thread_queue_init(READY);
    if(!ready_q.success){
        result.error_code = SCHED_READY_QUEUE_INIT_FAIL;
        return result;
    }
    thread_queue_init_out sleeping_q =  thread_queue_init(SLEEPING);
    if(!sleeping_q.success){
        result.error_code = SCHED_SLEEP_QUEUE_INIT_FAIL;
        return result;
    }
    thread_queue_init_out dying_q =  thread_queue_init(DYING);
    if(!dying_q.success){
        result.error_code = SCHED_DYING_QUEUE_INIT_FAIL;
        return result;
    }
    thread_queue_init_out keyboard_q =  thread_queue_init(KEYBOARD_WAIT);
    if(!keyboard_q.success){
        result.error_code = SCHED_KEYBOARD_WAIT_QUEUE_INIT_FAIL;
        return result;
    }


    cpu->current_thread = NULL;
    cpu->tick_count = 0;
    cpu->default_quantum_ticks = SCHEDULER_DEFAULT_QUANTUM_TICKS;
    cpu->is_initialized = true;
    cpu->dying_queue = dying_q.q;
    cpu->need_resched = false;
    cpu->ready_queue = ready_q.q;
    cpu->sleep_queue = sleeping_q.q;
    cpu->keyboard_wait_queue = keyboard_q.q;

    result.success = true;
    result.cpu_id = cpu_id;
    result.cpu = cpu;
    return result;
    


}


scheduler_cpu *scheduler_get_cpu(uint64_t cpu_id){

    if(cpu_id >= MAX_CPUS){return NULL;}

    scheduler_cpu *cpu = &cpus[cpu_id];
    if(!cpu->is_initialized || !cpu->present){return NULL;}

    return cpu;

}

scheduler_cpu *scheduler_this_cpu(void){
    //this is just a place holder for now and will be replaced with real cpu look up once SMP
    // implemntation starts 


    return scheduler_get_cpu(0);




} 



sched_add_ready_out scheduler_add_ready(uint64_t cpu_id ,thread *t){

    sched_add_ready_out result = {0};

    if(cpu_id >= MAX_CPUS) return result;

    if(t == NULL){

        result.error_code = INVALID_THREAD;
        return result;
    }

    uint64_t old_state = t->state;

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(cpu == NULL || !cpu->present || !cpu->is_initialized){return result;}

    if(cpu->ready_queue == NULL){

        result.error_code = SCHED_NOT_READY;
        
        
        return result;
    }

    if(t->owner_queue != NULL){
        
        result.error_code = THREAD_ALREADY_IN_OTHER_Q;
        return result;
    }

    t->state = READY_T;

    if(!thread_queue_push(cpu->ready_queue, t)){

        t->state = old_state;
        return result;
    }

    result.success = true;

    return result;



}


static uint64_t scheduler_add_dying(uint64_t cpu_id ,thread *t){

    

    if(cpu_id >= MAX_CPUS) return 0;

    if(t == NULL){return 0;}
    if(t->state != DYING_T){return 0;}
    


    scheduler_cpu *cpu = &cpus[cpu_id];

    if(cpu == NULL || !cpu->present || !cpu->is_initialized){return 0;}

    if(cpu->dying_queue == NULL){ return 0;}

    if(t->owner_queue != NULL){return 0;}


    t->state = DYING_T;

    if(!thread_queue_push(cpu->dying_queue, t)){return 0;}

    

    return 1;



}


thread *scheduler_pick_next(uint64_t cpu_id){

    if(cpu_id >= MAX_CPUS) return NULL;

    

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(cpu == NULL || !cpu->present || !cpu->is_initialized){return NULL;}

    if(cpu->ready_queue == NULL){return NULL;}

    thread_queue_pop_out tmd = thread_queue_pop(cpu->ready_queue);
    if(!tmd.success){return NULL;}

    thread *t = tmd.popped_t;

    t->state = RUNNING_T;

    return t;

    

    


}


uint64_t scheduler_set_current(uint64_t cpu_id ,thread *t){

    if(cpu_id >= MAX_CPUS) return 0;


    scheduler_cpu *cpu = &cpus[cpu_id];

    if(cpu == NULL || !cpu->present || !cpu->is_initialized){return 0;}

    cpu->current_thread = t;

    return 1;

    



}


thread *scheduler_current(uint64_t cpu_id){


    if(cpu_id >= MAX_CPUS) return NULL;

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(cpu == NULL || !cpu->present || !cpu->is_initialized){return NULL;}

    return cpu->current_thread;



}


scheduler_yield_out scheduler_yield(uint64_t cpu_id){

    scheduler_yield_out result = {0};

    if(cpu_id >= MAX_CPUS){
        result.error_code = SCHED_NOT_READY;
        return result;
    }

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(!cpu->present || !cpu->is_initialized){
        result.error_code = SCHED_NOT_READY;
        return result;
    }

    if(cpu->ready_queue == NULL){
        result.error_code = SCHED_NOT_READY;
        return result;
    }

    thread *current = cpu->current_thread;

    if(current == NULL){ // IDLE FALL BACK HERE TOO
        thread *next = scheduler_pick_next(cpu_id);
        if(next == NULL){
            result.error_code = SCHED_NOT_READY;
            return result;
        }

        if(!scheduler_set_current(cpu_id, next)){
            result.error_code = SCHED_NOT_READY;
            return result;
        }

        result.success = true;
        return result;
    }

    result.yielded_t = current;

    sched_add_ready_out add_current = scheduler_add_ready(cpu_id, current);
    if(!add_current.success){
        result.error_code = add_current.error_code;
        return result;
    }

    thread *next = scheduler_pick_next(cpu_id);
    if(next == NULL){ // this needs to be patched so that it runs the idle thread

        thread_queue_remove_out removed_md = thread_queue_remove(cpu->ready_queue, current);
        if(!removed_md.success){return result;} // these two lines are additions made later

        scheduler_set_current(cpu_id, current);
        
        result.error_code = SCHED_NOT_READY;
        return result;
    }

    if(!scheduler_set_current(cpu_id, next)){ // idfk abt this condition 

        sched_add_ready_out add_ready_md = scheduler_add_ready(cpu->cpu_id ,next);
        if(add_ready_md.success == false) return result;
        thread_queue_remove_out remove_md = thread_queue_remove(cpu->ready_queue, current);
        if(remove_md.success == false){return result;}
        current->state = RUNNING_T;





        scheduler_set_current(cpu_id, current);
        result.error_code = SCHED_NOT_READY;
        return result;
    }

    result.success = true;
    return result;

}


uint64_t scheduler_tick(uint64_t cpu_id){

    if(cpu_id >= MAX_CPUS){return 0;}

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(!cpu->present || !cpu->is_initialized){return 0;}

    cpu->tick_count++;

    if(cpu->current_thread == NULL){return 1;}

    if(cpu->default_quantum_ticks == 0){return 1;}

    if(cpu->tick_count % cpu->default_quantum_ticks == 0){
        cpu->need_resched = true;
    }

    return 1;

}


uint64_t scheduler_exit_current(uint64_t cpu_id){

    if(cpu_id >= MAX_CPUS){return 0;}

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(!cpu->present || !cpu->is_initialized){return 0;}
    if(cpu->current_thread == NULL){return 0;}

   thread_exit_out exit_md = thread_exit(cpu->current_thread, cpu->current_thread->exit_status);

   if(!exit_md.success){return 0;}
   thread *exiting_thread = exit_md.exiting_thread;

   if(!scheduler_add_dying(cpu->cpu_id, exiting_thread)){return 0;}

   thread *next = scheduler_pick_next(cpu->cpu_id);
   if(next == NULL){return 0;} ///// idle thread may be postponded to init

   if(!scheduler_set_current(cpu->cpu_id, next)){return 0;}

   return 1;


}


uint64_t scheduler_block_current(uint64_t cpu_id, thread_queue *wait_queue){

    if(cpu_id >= MAX_CPUS){return 0;}

    scheduler_cpu *cpu = &cpus[cpu_id];

    if(!cpu->present || !cpu->is_initialized){return 0;}
    if(wait_queue == NULL){return 0;}

    thread *current = cpu->current_thread;
    if(current == NULL){return 0;}
    if(current->owner_queue != NULL){return 0;}

    current->state = BLOCKED_T;

    if(!thread_queue_push(wait_queue, current)){
        current->state = RUNNING_T;
        return 0;
    }

    cpu->current_thread = NULL;

    return 1;

}


uint64_t scheduler_wake_one(uint64_t cpu_id, thread_queue *wait_queue){

    if(cpu_id >= MAX_CPUS){return 0;}
    if(wait_queue == NULL){return 0;}

    thread_queue_pop_out popped_md = thread_queue_pop(wait_queue);
    if(!popped_md.success){return 0;}

    sched_add_ready_out add_md = scheduler_add_ready(cpu_id, popped_md.popped_t);

    return add_md.success ? 1 : 0;

}


uint64_t scheduler_wake_all(uint64_t cpu_id, thread_queue *wait_queue){

    if(cpu_id >= MAX_CPUS){return 0;}
    if(wait_queue == NULL){return 0;}

    uint64_t woken = 0;

    while(wait_queue->thread_count > 0){
        if(!scheduler_wake_one(cpu_id, wait_queue)){break;}
        woken++;
    }

    return woken;

}



