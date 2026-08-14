#include <vmm.h>
#include <pmm.h>
#include <pic.h>
#include <keyboard.h>
#include <handlers.h>
#include <serial.h>


void panic(const char *msg){

    printk("stop fucking around");
    printk("u chose this");
    printk("dont act surprised its not working");
    printk(msg);


    for(;;){ __asm__ volatile("hlt"); }

     

}


static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}


static volatile uint64_t timer_ticks = 0;

void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip){

    uint64_t err_code = error_code & 0x7;
    const char *msg;

    

    if(err_code == 0x0){

        msg = "kenrel read hit a page that was not present error";

        //a kernel read hit a page that was not present.

        uintptr_t alligned_va = faulting_va & ~0xFFFULL;
        //get curr active space from scvheduelr
        scheduler_cpu *this_cpu = scheduler_this_cpu();
        if(this_cpu == NULL){ panic(msg); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ panic(msg); }
        if(curr_t->owning_proc == NULL){ panic(msg); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ panic(msg); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ panic(msg); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ panic(msg); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); panic(msg); }
        

    }






    if(err_code == 0x1){

        //a kernel read hit a page that was present but violated protection.
        msg = "a kernel read hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a kernel read hit a page that was present but violated protection NX CASE.";

        }
        panic(msg);
        
    }







    if(err_code == 0x2){

        // this code is a duplicate of the first case but we need to isolate for future additions
        // just to be safe 

        msg = "kenrel read hit a page that was not present error";

        //a kernel write hit a page that was not present.

         uintptr_t alligned_va = faulting_va & ~0xFFFULL;
        //get curr active space from scvheduelr
        scheduler_cpu *this_cpu = scheduler_this_cpu();
        if(this_cpu == NULL){ panic(msg); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ panic(msg); }
        if(curr_t->owning_proc == NULL){ panic(msg); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ panic(msg); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ panic(msg); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ panic(msg); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); panic(msg); }

    }






    if(err_code == 0x3){

        //a kernel write hit a page that was present but violated protection.

          
        msg = "a kernel write hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a kernel write hit a page that was present but violated protection NX CASE.";

        }
        panic(msg);

    }






    if(err_code == 0x4){

        //a user read hit a page that was not present.


    }

       if(err_code == 0x5){

        //a user read hit a page that was present but violated protection.

        if(error_code & PFEC_ID){ //nx case



        }


    }







       if(err_code == 0x6){

        //a user write hit a page that was not present.


    }








       if(err_code == 0x7){

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

