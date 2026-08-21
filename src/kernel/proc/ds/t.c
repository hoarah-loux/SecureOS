#include <t.h>


static uint64_t tid_count = 1;
static uint64_t tcount = 1;

//rip in this fucntion means the entry point were the therad would tart executing
create_thread_out create_thread(proc *owning_process, uintptr_t rip, uint64_t thread_flags, size_t user_stack_size){


    create_thread_out result = {0};

    if(owning_process == NULL){return result;}
    if(rip == 0){return result;}
    
    
    bool t_k = false;

    if(owning_process->flags & PROC_F_KERNEL){ t_k = true; }

    if(!t_k && user_stack_size == 0){return result;}

    thread *t = kcalloc(1, sizeof(thread));
    if(t == NULL){return result;}

    vm_space *space = owning_process->adress_space;
    if(space == NULL){return result;}
    vm_object *object_stack_usr = NULL;
    vm_region *usr_stack = NULL;
    vm_region *ker_stack = NULL;
    
    


    if(!t_k){

        object_stack_usr = kcalloc(1, sizeof(vm_object));
        if(object_stack_usr == NULL){
            kfree(t);
            result.success = false;
            result.error_code = USER_STACK_RESERVE_ERROR;
            return result;
        }

        object_stack_usr->type = ANON;
        object_stack_usr->backing_ctx = NULL;
        object_stack_usr->size_bytes = user_stack_size;
        object_stack_usr->ref_count = 0;

        reserve_region_request stack_request_usr = {

            .size = user_stack_size,
            .type = STACK,
            .map_flags = TOPDOWN | GROWDOWN | PRIVATE,
            .perms =  USER_STACK_FLAGS,
            .object = object_stack_usr
          
        };

        reserve_region_request *req_u = &stack_request_usr;

        reserve_region_out usr_stack_md = reserve_region(space, req_u);
        if(!usr_stack_md.success){
            kfree(object_stack_usr);
            kfree(t);
            result.success = false;
            result.error_code = USER_STACK_RESERVE_ERROR;
            return result;

        }

        usr_stack = usr_stack_md.region_ptr;

        uintptr_t u_stack_va_start = usr_stack->va_start;
        uintptr_t u_stack_va_end = usr_stack->va_end;

        t->_rsp_u = u_stack_va_end; // STACKS FROWN DOWN SO THEY START AT VA END.....
        t->user_stack_va_start = u_stack_va_start;
        t->user_stack_va_end = u_stack_va_end;
        t->user_stack_size = usr_stack->size_rounded;
        t->usr_stack_region_ptr = usr_stack;
        t->t_region_list = usr_stack;

    }
    

    // after createing destroy region path dont forget to destroy and free the usr stack if 
    // the kernel stack allocation fails in the user path.


    reserve_region_request stack_request_ker = {

        .size = 16 * 1024,
        .type = STACK,
        .map_flags = KERNEL_ASSET | PRIVATE,
        .perms = KERNEL_STACK_FLAGS

    };


    reserve_region_request *req_k = &stack_request_ker;

    reserve_region_out ker_stack_md = reserve_region(space, req_k);
    if(!ker_stack_md.success){
        if(usr_stack != NULL){
            (void)destroy_vm_region(space, usr_stack);
        }
        kfree(t);
        result.success = false;
        result.error_code = KERNEL_STACK_RESERVE_ERROR;
        return result;


    }
    ker_stack = ker_stack_md.region_ptr;

    uintptr_t k_stack_va_start = ker_stack->va_start;
    uintptr_t k_stack_va_end = ker_stack->va_end;

    t->_rsp_k = k_stack_va_end;
    t->kernel_stack_size = 16 * 1024;
    t->kernel_stack_va_end = k_stack_va_end;
    t->kernel_stack_va_start = k_stack_va_start;
    t->ker_stack_region_ptr = ker_stack;
    t->t_k_a_region_list = ker_stack;

    //fill put the rest of the metadata

    t->id = tid_count; //counter of some type we will see
    tid_count += 1;
    tcount ++;
    t->next = NULL;
    t->prev = NULL;
    t->q_thread_next = NULL;
    t->q_thread_prev = NULL;
    t->state = NEW_T;
    t->flags = thread_flags;
    t->owning_proc = owning_process;
    t->is_kernel = t_k;
    t->ref_count = 1;



    if(t->flags & THREAD_F_MAIN_THREAD){
        
        owning_process->main_thread = t;

    }




    context *c = kcalloc(1, sizeof(context));
    if(c == NULL){
        if(ker_stack != NULL){
            (void)destroy_vm_region(space, ker_stack);
        }
        if(usr_stack != NULL){
            (void)destroy_vm_region(space, usr_stack);
        }
        kfree(t);
        result.error_code = CONTEXT_ALLOC_FAIL;
        result.success = false;        
        return result;
    }

    if(t_k){
        c->k = true;
        c->owner_thread = t;
        c->cr3 = owning_process->adress_space->pml4_phys;
        c->rip = rip; 
        c->kernel_rsp = t->_rsp_k;
        c->rflags = 0x202; 
        c->cs = 0x08;
        c->ss = 0x10;
        t->context = c;
    }else{
        c->k = false;
        c->owner_thread = t;
        c->cr3 = owning_process->adress_space->pml4_phys;
        c->rip = rip; 
        c->user_rsp = t->_rsp_u;
        c->kernel_rsp = t->_rsp_k;
        c->rflags = 0x202;
        c->cs = 0x1B;
        c->ss = 0x23; 
        t->context = c;



    }

    // appened to thread list 

    //kernel list

    

    


    //append to procs thread list

    thread *thread_list_cursor = owning_process->thread_head;

    if(thread_list_cursor == NULL){

        owning_process->thread_head = t;
        owning_process->thread_tail = t;
        owning_process->thread_count += 1;

    }

    while(thread_list_cursor != NULL){

        if(thread_list_cursor->next == NULL){

            thread_list_cursor->next = t;
            t->prev = thread_list_cursor;
            owning_process->thread_tail = t;
            owning_process->thread_count += 1;
            break;

        }

        thread_list_cursor = thread_list_cursor->next;

    }

    if(thread_flags & THREAD_F_START_SUSPENDED){

        t->state = BLOCKED_T;

    }else{

        t->state = NEW_T;

    }

    result.success = true;
    result.thread = t;
    result.error_code = 0;

    return result;


}


thread_exit_out thread_exit(thread *t, uint64_t exit_status){

    thread_exit_out result = {0};

    if(t == NULL){
        result.error_code = THREAD_EXIT_INVALID_THREAD;
        return result;
    }

    if(t->owning_proc == NULL){
        result.error_code = THREAD_EXIT_INVALID_OWNER;
        return result;
    }

    proc *owner = t->owning_proc;

    if(t->ref_count == 0){
        result.error_code = THREAD_EXIT_INVALID_REFCOUNT;
        return result;
    }

    if(t->flags & THREAD_F_REAPED){
        result.error_code = THREAD_EXIT_ALREADY_REAPED;
        return result;
    }

    if(t->state == DYING_T || (t->flags & THREAD_F_REAPABLE)){
        result.error_code = THREAD_EXIT_ALREADY_DYING;
        return result;
    }

    if(t->state != RUNNING_T){
        result.error_code = THREAD_EXIT_NOT_RUNNING;
        return result;
    }

    if(t->owner_queue != NULL){
        result.error_code = THREAD_EXIT_STILL_QUEUED;
        return result;
    }

    if(owner->thread_count == 1){
        if(owner->thread_head != t || owner->thread_tail != t){
            result.error_code = THREAD_EXIT_CORRUPT_PROCESS_LIST;
            return result;
        }

        proc_exit_out proc_exit_md = proc_exit(owner);
        if(!proc_exit_md.success){
            result.error_code = THREAD_EXIT_PROC_EXIT_FAILED;
            return result;
        }
    }

    t->exit_status = exit_status;
    t->state = DYING_T;
    t->flags |= THREAD_F_REAPABLE;

    result.exiting_thread = t;
    result.success = true;
    result.error_code = 0;
    return result;
}


reap_thread_out reap_thread(thread *t){

    reap_thread_out result = {0};

    if(t == NULL){
        result.error_code = THREAD_REAP_INVALID_THREAD;
        return result;
    }

    proc *owner = t->owning_proc;
    if(owner == NULL){
        result.error_code = THREAD_REAP_INVALID_OWNER;
        return result;
    }

    if(t->ref_count == 0){
        result.error_code = THREAD_REAP_INVALID_REFCOUNT;
        return result;
    }

    if(t->flags & THREAD_F_REAPED){
        result.error_code = THREAD_REAP_ALREADY_REAPED;
        return result;
    }

    if(t->state != DYING_T){
        result.error_code = THREAD_REAP_NOT_DYING;
        return result;
    }

    if(!(t->flags & THREAD_F_REAPABLE)){
        result.error_code = THREAD_REAP_NOT_REAPABLE;
        return result;
    }

    if(t->owner_queue != NULL){
        result.error_code = THREAD_REAP_STILL_QUEUED;
        return result;
    }

    if(owner->thread_count == 0 || owner->thread_head == NULL || owner->thread_tail == NULL){
        result.error_code = THREAD_REAP_EMPTY_PROCESS_LIST;
        return result;
    }

    bool found = false;
    thread *cursor = owner->thread_head;
    for(uint64_t visited = 0; cursor != NULL && visited < owner->thread_count; visited++){
        if(cursor == t){
            found = true;
            break;
        }
        cursor = cursor->next;
    }

    if(!found){
        result.error_code = THREAD_REAP_NOT_IN_PROCESS;
        return result;
    }

    if((t->prev == NULL && owner->thread_head != t) ||
       (t->prev != NULL && t->prev->next != t) ||
       (t->next == NULL && owner->thread_tail != t) ||
       (t->next != NULL && t->next->prev != t)){
        result.error_code = THREAD_REAP_CORRUPT_PROCESS_LIST;
        return result;
    }

    bool was_last_thread = (owner->thread_count == 1);

    if(t->prev != NULL){
        t->prev->next = t->next;
    }else{
        owner->thread_head = t->next;
    }

    if(t->next != NULL){
        t->next->prev = t->prev;
    }else{
        owner->thread_tail = t->prev;
    }

    if(owner->main_thread == t){
        owner->main_thread = NULL;
    }

    owner->thread_count--;

    if(owner->thread_count == 0){
        owner->thread_head = NULL;
        owner->thread_tail = NULL;
    }

    t->next = NULL;
    t->prev = NULL;
    t->q_thread_next = NULL;
    t->q_thread_prev = NULL;
    t->flags &= ~THREAD_F_REAPABLE;
    t->flags |= THREAD_F_REAPED;

    if(was_last_thread){
        reap_proc_out proc_reap_md = reap_proc(owner);
        if(!proc_reap_md.success){
            result.error_code = THREAD_REAP_PROC_REAP_FAILED;
            return result;
        }
    }

    result.reaped_thread = t;
    result.exit_status = t->exit_status;
    result.was_last_thread = was_last_thread;
    result.success = true;
    result.error_code = 0;
    return result;
}


destroy_thread_out destroy_thread(thread *t){

    destroy_thread_out result = {0};

    if(t == NULL){
        result.error_code = THREAD_DESTROY_INVALID_THREAD;
        return result;
    }

    proc *owner = t->owning_proc;
    if(owner == NULL){
        result.error_code = THREAD_DESTROY_INVALID_OWNER;
        return result;
    }

    if(t->ref_count != 1){
        result.error_code = THREAD_DESTROY_INVALID_REFCOUNT;
        return result;
    }

    if(t->state != DYING_T){
        result.error_code = THREAD_DESTROY_NOT_DYING;
        return result;
    }

    if(!(t->flags & THREAD_F_REAPED)){
        result.error_code = THREAD_DESTROY_NOT_REAPED;
        return result;
    }

    if(t->flags & THREAD_F_REAPABLE){
        result.error_code = THREAD_DESTROY_STILL_REAPABLE;
        return result;
    }

    if(t->owner_queue != NULL || t->q_thread_next != NULL || t->q_thread_prev != NULL){
        result.error_code = THREAD_DESTROY_STILL_QUEUED;
        return result;
    }

    if(t->next != NULL || t->prev != NULL ||
       owner->thread_head == t || owner->thread_tail == t || owner->main_thread == t){
        result.error_code = THREAD_DESTROY_STILL_PROCESS_ATTACHED;
        return result;
    }

    thread *cursor = owner->thread_head;
    for(uint64_t visited = 0; cursor != NULL && visited < owner->thread_count; visited++){
        if(cursor == t){
            result.error_code = THREAD_DESTROY_STILL_PROCESS_ATTACHED;
            return result;
        }
        cursor = cursor->next;
    }

    if(owner->adress_space == NULL){
        result.error_code = THREAD_DESTROY_INVALID_ADDRESS_SPACE;
        return result;
    }

    if(t->ker_stack_region_ptr == NULL){
        result.error_code = THREAD_DESTROY_INVALID_KERNEL_STACK_REGION;
        return result;
    }

    if(!t->is_kernel && t->usr_stack_region_ptr == NULL){
        result.error_code = THREAD_DESTROY_INVALID_USER_STACK_REGION;
        return result;
    }

    if(t->context == NULL){
        result.error_code = THREAD_DESTROY_INVALID_CONTEXT;
        return result;
    }

    if(t->context->owner_thread != t){
        result.error_code = THREAD_DESTROY_CONTEXT_OWNER_MISMATCH;
        return result;
    }

    uint64_t destroyed_thread_id = t->id;


    //destroy normal list
    vm_region *cursor_n = t->t_region_list;

    while(cursor_n != NULL){

        vm_region *next_n = cursor_n->next_t;

        if(!destroy_vm_region(owner->adress_space, cursor_n)){

            result.success = false;
            result.error_code = REGION_DESTROY_FAIL;
            return result;
        }

        cursor_n = next_n;



    }

    //destroy kernel list

    vm_region *cursor_k = t->t_k_a_region_list;

    while(cursor_k != NULL){

        vm_region *next_k = cursor_k->next_t;

        if(!destroy_vm_region(owner->adress_space, cursor_k)){

            result.success = false;
            result.error_code = REGION_DESTROY_FAIL;
            return result;


        }

        cursor_k = next_k;




    }




    t->context->owner_thread = NULL;
    if(!kfree(t->context)){
        result.error_code = THREAD_DESTROY_CONTEXT_FREE_FAILED;
        return result;
    }

    t->context = NULL;
    t->owning_proc = NULL;
    t->ker_stack_region_ptr = NULL;
    t->usr_stack_region_ptr = NULL;
    t->ref_count = 0;

    if(!kfree(t)){
        result.error_code = THREAD_DESTROY_THREAD_FREE_FAILED;
        return result;
    }

    if(owner->thread_count == 0 &&
       owner->thread_head == NULL &&
       owner->thread_tail == NULL &&
       owner->main_thread == NULL){
        (void)destroy_proc(owner);
    }

    result.destroyed_thread_id = destroyed_thread_id;
    result.success = true;
    result.error_code = 0;
    return result;
}
