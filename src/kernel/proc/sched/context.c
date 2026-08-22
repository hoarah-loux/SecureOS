#include <context.h>
#include <t.h>



save_trap_frame_out save_trap_frame(trap_frame *frame, thread *t){

    save_trap_frame_out result = {0};

    if(frame == NULL || t == NULL || t->context == NULL){return result;}

    // reuse the thread's own context object instead of allocating a fresh
    // one every trap - this runs on every preemption/yield, so a kcalloc()
    // here leaked two heap objects per context switch.
    context *context_save = t->context;

    if((frame->cs & 3) == 3){

        context_save->k = false;
        context_save->r15 = frame->r15;
        context_save->r14 = frame->r14;
        context_save->r13 = frame->r13;
        context_save->r12 = frame->r12;
        context_save->r11 = frame->r11;
        context_save->r10 = frame->r10;
        context_save->r9 = frame->r9;
        context_save->r8 = frame->r8;
        context_save->rbp = frame->rbp;
        context_save->rdi = frame->rdi;
        context_save->rsi = frame->rsi;
        context_save->rdx = frame->rdx;
        context_save->rcx = frame->rcx;
        context_save->rbx = frame->rbx;
        context_save->rax = frame->rax;
        context_save->rip = frame->rip;
        context_save->cs = frame->cs;
        context_save->rflags = frame->rflags;
        context_save->user_rsp = frame->rsp;
        context_save->ss = frame->ss;
        context_save->kernel_rsp = t->_rsp_k;
        context_save->cr3 = t->owning_proc->adress_space->pml4_phys;
        context_save->owner_thread = t;

    }else{

        context_save->k = true;
        context_save->r15 = frame->r15;
        context_save->r14 = frame->r14;
        context_save->r13 = frame->r13;
        context_save->r12 = frame->r12;
        context_save->r11 = frame->r11;
        context_save->r10 = frame->r10;
        context_save->r9 = frame->r9;
        context_save->r8 = frame->r8;
        context_save->rbp = frame->rbp;
        context_save->rdi = frame->rdi;
        context_save->rsi = frame->rsi;
        context_save->rdx = frame->rdx;
        context_save->rcx = frame->rcx;
        context_save->rbx = frame->rbx;
        context_save->rax = frame->rax;
        context_save->rip = frame->rip;
        context_save->cs = frame->cs;
        context_save->rflags = frame->rflags;
        context_save->user_rsp = 0;
        context_save->ss = 0;
        context_save->kernel_rsp = (uintptr_t)&frame->rflags + sizeof(uint64_t);
        context_save->cr3 = t->owning_proc->adress_space->pml4_phys;
        context_save->owner_thread = t;

    }

    result.context_saved = context_save;
    result.frame_saved = NULL; // no longer allocated; unused by any current caller
    result.success = true;

    return result;




}
