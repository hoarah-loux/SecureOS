#include <t.h>




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


    if(!t_k){

        reserve_region_request stack_request_usr = {

            .size = user_stack_size,
            .type = STACK,
            .map_flags = TOPDOWN | GROWDOWN | PRIVATE,
          
        };

        reserve_region_request *req_u = &stack_request_usr;

        reserve_region_out usr_stack_md = reserve_region(space, req_u);
        if(!usr_stack_md.success){
            kfree(t);
            result.success = false;
            result.error_code = USER_STACK_RESERVE_ERROR;
            return result;

        }

        vm_region *usr_stack = usr_stack_md.region_ptr;

        uintptr_t u_stack_va_start = usr_stack->va_start;
        uintptr_t u_stack_va_end = usr_stack->va_end;

        t->_rsp_u = u_stack_va_end; // STACKS FROWN DOWN SO THEY START AT VA END.....
        t->user_stack_va_start = u_stack_va_start;
        t->user_stack_va_end = u_stack_va_end;
        t->user_stack_size = usr_stack->size_rounded;

    }
    

    // after createing destroy region path dont forget to destroy and free the usr stack if 
    // the kernel stack allocation fails in the user path.


    reserve_region_request stack_request_ker = {

        .size = 16 * 1024,
        .type = STACK,
        .map_flags = KERNEL_ASSET | PRIVATE,

    };


    reserve_region_request *req_k = &stack_request_ker;

    reserve_region_out ker_stack_md = reserve_region(space, req_k);
    if(!ker_stack_md.success){
        kfree(t);
        result.success = false;
        result.error_code = KERNEL_STACK_RESERVE_ERROR;
        return result;


    }

    vm_region *ker_stack = ker_stack_md.region_ptr;

    uintptr_t k_stack_va_start = ker_stack->va_start;
    uintptr_t k_stack_va_end = ker_stack->va_end;

    t->_rsp_k = k_stack_va_end;
    t->kernel_stack_size = 16 * 1024;
    t->kernel_stack_va_end = k_stack_va_end;
    t->kernel_stack_va_start = k_stack_va_start;

    //fill put the rest of the metadata

    t->id = 0; //counter of some type we will see
    t->next = NULL;
    t->prev = NULL;
    t->rq_thread_next = NULL;
    t->rq_thread_prev = NULL;
    t->state = NEW;
    t->flags = thread_flags;
    t->owning_proc = owning_process;
    t->is_kernel = t_k;

    if(t->flags & THREAD_F_MAIN_THREAD){
        
        owning_process->main_thread = t;

    }




    context *c = kcalloc(1, sizeof(context));
    if(c == NULL){
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

        t->state = BLOCKED;

    }else{

        t->state = READY;

    }

    result.success = true;
    result.thread = t;
    result.error_code = 0;

    return result;





}