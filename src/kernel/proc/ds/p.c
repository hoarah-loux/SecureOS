#include <p.h>

uintptr_t *k_root;

create_proc_out create_proc(bool k){

    create_proc_out result = {0};
    //allocate a proc struct on the kheap
    proc *p = kcalloc(1,sizeof(proc));

    if(p == NULL){
        return result;
    }

    create_vm_space_out space_md = create_vm_space(k_root);
    if(space_md.success == false){
        kfree(p);
        return result;
    }

    p->id = 0;
    p->parent_pid = 0;
    p->next = NULL;
    p->prev = NULL;

    p->adress_space = space_md.space;
    p->main_thread = NULL;
    p->thread_list = NULL;
    p->thread_head = NULL;
    p->thread_tail = NULL;

    p->state = NEW;
    if(k == true){p->flags = PROC_F_KERNEL;}
    else{p->flags = PROC_F_USER;}
    
    p->refcount = 1;
    p->lock = 0;
    p->thread_count = 0;
    p->exit_status = 0;

    p->child_head = NULL;
    p->child_tail = NULL;
    p->child_list = NULL;
    p->sibling_prev = NULL;
    p->sibling_next = NULL;
    p->parent = NULL;

    result.proc = p;
    result.success = true;
    result.error_code = 0;

    return result;
    
    
}

destroy_proc_out destroy_proc(proc *p){

    destroy_proc_out result = {0};
    if(p == NULL)return result;
    if(p->thread_list != NULL){

        result.error_code = THREAD_ACTIVE;
        result.success = false;
        return result;
    }

    if(p->refcount > 1){
        result.error_code = PROCESS_IS_REFED;
        result.success = false;
        return result;

    }

    if(p->state != TERMINATED || !(p->flags & PROC_F_REAPED)){
        result.error_code = PROC_NOT_TERMINATED;
        result.success = false;
        return result;
    }

    if(p->adress_space != NULL){

        //destroy space
    }

    if(p->child_list != NULL){

        result.error_code = PROC_STILL_HAS_CHILD;
        result.success = false;
        return result;
    }

    if(p->prev != NULL){
    p->prev->next = p->next;
    }

    if(p->next != NULL){
    p->next->prev = p->prev;
    }

    if(p->sibling_prev != NULL){
    p->sibling_prev->sibling_next = p->sibling_next;
    }
    
    if(p->sibling_next != NULL){
    p->sibling_next->sibling_prev = p->sibling_prev;
    }





    p->child_head = NULL;
    p->child_list = NULL;
    p->child_tail = NULL;
    


    
    









    kfree(p);
    result.error_code = 0;
    result.success = true;
    return result;


}



proc_exit_out proc_exit(proc *p){

    proc_exit_out result = {0};

    if(p == NULL){return result;}

    if(p->flags & PROC_F_REAPED){
        result.error_code  = ALREADY_REAPED;
        result.success = false;
        return result;

    }

    p->flags = p->flags & PROC_F_EXITING;
    p->state = TERMINATED;
    p->flags = p->flags & PROC_F_REAPABLE;
    p->exit_status = 0;
    






}
