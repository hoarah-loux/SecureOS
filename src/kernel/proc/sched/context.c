#include <context.h>



save_trap_frame_out save_trap_frame(trap_frame *frame, thread *t){

    save_trap_frame_out result = {0};

    if(frame == NULL || t == NULL ){return result;}

    trap_frame *frame_save = kcalloc(1, sizeof(trap_frame));
    context *context_save = kcalloc(1, sizeof(context));

    

    


    if((frame->cs & 3) == 3){

        frame_save->r15 = frame->r15;
        frame_save->r14 = frame->r14;
        frame_save->r13 = frame->r13;
        frame_save->r12 = frame->r12;
        frame_save->r11 = frame->r11;
        frame_save->r10 = frame->r10;
        frame_save->r9 = frame->r9;
        frame_save->r8 = frame->r8;
        frame_save->rbp = frame->rbp;
        frame_save->rdi = frame->rdi;
        frame_save->rsi = frame->rsi;
        frame_save->rdx = frame->rdx;
        frame_save->rcx = frame->rcx;
        frame_save->rbx = frame->rbx;
        frame_save->rax = frame->rax;
        frame_save->rip = frame->rip;
        frame_save->cs = frame->cs;
        frame_save->rflags = frame->rflags;
        frame_save->rsp = frame->rsp;
        frame_save->ss = frame->ss;


        context_save->k = false;
        context_save->r15 = frame_save->r15;
        context_save->r14 = frame_save->r14;
        context_save->r13 = frame_save->r13;
        context_save->r12 = frame_save->r12;
        context_save->r11 = frame_save->r11;
        context_save->r10 = frame_save->r10;
        context_save->r9 = frame_save->r9;
        context_save->r8 = frame_save->r8;
        context_save->rbp = frame_save->rbp;
        context_save->rdi = frame_save->rdi;
        context_save->rsi = frame_save->rsi;
        context_save->rdx = frame_save->rdx;
        context_save->rcx = frame_save->rcx;
        context_save->rbx = frame_save->rbx;
        context_save->rax = frame_save->rax;
        context_save->rip = frame_save->rip;
        context_save->cs = frame_save->cs;
        context_save->rflags = frame_save->rflags;
        context_save->user_rsp = frame_save->rsp;
        context_save->ss = frame_save->ss;
        context_save->kernel_rsp = t->_rsp_k;
        context_save->cr3 = t->owning_proc->adress_space->pml4_phys;
        context_save->owner_thread = t;

        

    }else{

        frame_save->r15 = frame->r15;
        frame_save->r14 = frame->r14;
        frame_save->r13 = frame->r13;
        frame_save->r12 = frame->r12;
        frame_save->r11 = frame->r11;
        frame_save->r10 = frame->r10;
        frame_save->r9 = frame->r9;
        frame_save->r8 = frame->r8;
        frame_save->rbp = frame->rbp;
        frame_save->rdi = frame->rdi;
        frame_save->rsi = frame->rsi;
        frame_save->rdx = frame->rdx;
        frame_save->rcx = frame->rcx;
        frame_save->rbx = frame->rbx;
        frame_save->rax = frame->rax;
        frame_save->rip = frame->rip;
        frame_save->cs = frame->cs;
        frame_save->rflags = frame->rflags;
        frame_save->rsp = (uintptr_t)&frame->rflags + sizeof(uint64_t);
        frame_save->ss = 0;


        context_save->k = true;
        context_save->r15 = frame_save->r15;
        context_save->r14 = frame_save->r14;
        context_save->r13 = frame_save->r13;
        context_save->r12 = frame_save->r12;
        context_save->r11 = frame_save->r11;
        context_save->r10 = frame_save->r10;
        context_save->r9 = frame_save->r9;
        context_save->r8 = frame_save->r8;
        context_save->rbp = frame_save->rbp;
        context_save->rdi = frame_save->rdi;
        context_save->rsi = frame_save->rsi;
        context_save->rdx = frame_save->rdx;
        context_save->rcx = frame_save->rcx;
        context_save->rbx = frame_save->rbx;
        context_save->rax = frame_save->rax;
        context_save->rip = frame_save->rip;
        context_save->cs = frame_save->cs;
        context_save->rflags = frame_save->rflags;
        context_save->user_rsp = 0;
        context_save->ss = frame_save->ss;
        context_save->kernel_rsp = frame_save->rsp;
        context_save->cr3 = t->owning_proc->adress_space->pml4_phys;
        context_save->owner_thread = t;




    }

    // we are leaking memory here need to alloc to the already exiositing thing 
    t->context = context_save;
    result.context_saved = context_save;
    result.frame_saved = frame_save;
    result.success = true;

    return result;









}