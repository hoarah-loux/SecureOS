#include <t.h>




//rip in this fucntion means the entry point were the therad would tart executing
create_thread_out create_thread(proc *owning_process, uintptr_t rip, uint64_t thread_flags, size_t user_stack_size){


    create_thread_out result = {0};

    if(owning_process == NULL){return result;}
    if(rip == NULL){return result;}
    if(thread_flags == NULL){return result;}
    
    bool t_k = false;

    if(owning_process->flags & PROC_F_KERNEL){ t_k = true; }

    if(!t_k && user_stack_size == NULL){return result;}

    thread *t = kcalloc(1, sizeof(thread));
    if(t == NULL){return result;}

    vm_space *space = owning_process->adress_space;

    reserve_region_request stack_request_usr = {

        .size = user_stack_size,
        .type = STACK,
        .map_flags = TOPDOWN | GROWDOWN | PRIVATE,
          
    };

    reserve_region_request *req_u = &stack_request_usr;

    reserve_region_out usr_stack_md = reserve_region(space, req_u);

    vm_region *usr_stack = usr_stack_md.region_ptr;

    uintptr_t *u_stack_va_start = (uintptr_t *)usr_stack->va_start;
    uintptr_t *u_stack_va_end = (uintptr_t *)usr_stack->va_end;

    
    


    reserve_region_request stack_request_ker = {

        .size = 16 * 1024,
        .type = STACK,
        .map_flags = KERNEL_ASSET | PRIVATE,

    };


    reserve_region_request *req_k = &stack_request_ker;

    reserve_region_out ker_stack_md = reserve_region(space, req_k);

    vm_region *ker_stack = ker_stack_md.region_ptr;

    uintptr_t *k_stack_va_start = (uintptr_t *)ker_stack->va_start;
    uintptr_t *k_stack_va_end = (uintptr_t *)ker_stack->va_end;

    t->_rsp = k_stack_va_end;




    
    










}