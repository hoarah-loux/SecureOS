#include <vmm.h>
#include <pmm.h>
#include <pic.h>
#include <keyboard.h>
#include <handlers.h>


static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

vm_space active_space;
static volatile uint64_t timer_ticks = 0;

void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip){


    if(error_code == 0x0){

        //a kernel read hit a page that was not present.

    }

    if(error_code == 0x1){

        //kernel write hit a page that was not present.
        
    }

    if(error_code == 0x2){

        // a kernel read hit a page that was present but violated protection.

    }

    if(error_code == 0x3){

        //a kernel write hit a page that was present but violated protection.
    }

    if(error_code == 0x4){

        //a user read hit a page that was not present.


    }

       if(error_code == 0x5){

        //a user write hit a page that was not present.


    }

       if(error_code == 0x6){

        //a user read hit a page that was present but violated protection


    }



       if(error_code == 0x7){

        //a user write hit a page that was present but violated protection


    }
}

void keyboard_interrupt_handler(int irq, void *data) {
    (void)irq;
    (void)data;

    uint8_t scancode = inb(0x60);
    keyboard_handle_scancode(scancode);
    pic_send_eoi(PIC_KEYBOARD_IRQ);
}


void timer_handler(trap_frame *frame, trap_reason trap_reason){
    
    timer_ticks++;
    pic_send_eoi(PIC_TIMER_IRQ);
    scheduler_cpu *cpu = scheduler_this_cpu();
    if(cpu == NULL){return;}
    if(!scheduler_tick(cpu->cpu_id)){return;}

    uint64_t cpu_id = cpu->cpu_id;
    
   
    


    if(!cpu->need_resched){

        return;

    }else{

        //this is the context switch

       thread *t = cpu->current_thread;
       save_trap_frame_out frame_md = save_trap_frame(frame, t);
       if(frame_md.success == false){return;}
       sched_add_ready_out ready_md = scheduler_add_ready(cpu_id, t);
       if(ready_md.success == false){return;}
       thread *next_t = scheduler_pick_next(cpu_id);
       if(next_t == NULL){return;}
       if(!scheduler_set_current(cpu_id, next_t)){return;}
       cpu->need_resched = false;
       restore_context(next_t->context);



    }


    
}

uint64_t timer_ticks_get(void) {
    return timer_ticks;
}


void yield_handler(trap_frame *frame, trap_reason trap_reason){

    (void)trap_reason;

    scheduler_cpu *cpu = scheduler_this_cpu();
    if(cpu == NULL || frame == NULL){return;}

    thread *current = cpu->current_thread;
    if(current == NULL){return;}

    save_trap_frame_out frame_md = save_trap_frame(frame, current);
    if(!frame_md.success){return;}

    scheduler_yield_out yield_md = scheduler_yield(cpu->cpu_id);
    if(!yield_md.success){return;}

    thread *next = scheduler_current(cpu->cpu_id);
    if(next == NULL){return;}

    cpu->need_resched = false;
    restore_context(next->context);

}


void exit_handler(trap_frame *frame, trap_reason trap_reason){ // figure out were exit status comes from

    scheduler_cpu *cpu = scheduler_this_cpu();
    if(cpu == NULL){return;}

    if(!scheduler_exit_current(cpu->cpu_id)){return;}
    thread *next_t = scheduler_current(cpu->cpu_id);
    if(next_t == NULL){return;}
    restore_context(next_t->context);





}
