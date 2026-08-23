#include <vmm.h>
#include <pmm.h>
#include <pic.h>
#include <keyboard.h>
#include <handlers.h>
#include <serial.h>
#include <fs_syscalls.h>


void panic(const char *msg){

    printk("stop fucking around\n");
    printk("u chose this\n");
    printk("dont act surprised its not working\n");
    printk(msg);
    printk("\n");


    for(;;){ __asm__ volatile("hlt"); }

     

}


static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}


static volatile uint64_t timer_ticks = 0;

static uint64_t populate_fault_frame(vm_region *region, uintptr_t alligned_va, uintptr_t physical_frame) {
    if (region == NULL) {
        return 0;
    }

    uint8_t *frame_buf = (uint8_t *)phys_to_virt(physical_frame);
    if (frame_buf == NULL) {
        return 0;
    }

    // Zero first, unconditionally. Anonymous regions want a zero page, and a
    // file region only ever covers region->size_bytes of file - the rest of
    // the final page (an ELF segment's .bss tail, or just the slack of a
    // non-page-multiple length) must read as zero, not as whatever bytes
    // happen to follow in the file.
    for (uint64_t i = 0; i < 4096; i++) {
        frame_buf[i] = 0;
    }

    if (region->object != NULL &&
        region->object->type == FILE &&
        region->object->backing_ctx != NULL) {

        if (alligned_va < region->va_start) {
            return 0;
        }

        uint64_t page_delta = (uint64_t)(alligned_va - region->va_start);

        // Past the file-backed extent of the region: pure zero page, no read.
        if (page_delta >= (uint64_t)region->size_bytes) {
            return 1;
        }

        uint64_t file_offset = (uint64_t)region->object_base_offset + page_delta;
        if (file_offset < page_delta) {
            return 0;
        }

        uint64_t remaining = (uint64_t)region->size_bytes - page_delta;
        uint64_t to_read = (remaining < 4096) ? remaining : 4096;

        if (fs_mmap_read(region->object->backing_ctx, file_offset, frame_buf, to_read) < 0) {
            return 0;
        }

        return 1;
    }

    return 1;
}

// #GP, vector 13.
//
// Until this existed vector 13 had no gate at all, so every #GP escalated
// straight to #DF and then triple fault - a silent reboot with nothing on the
// serial line. There is no recovery path yet (nothing here can safely resume a
// thread that executed an illegal operation), so this is a diagnostic: print
// enough to identify the fault, then stop.
//
// error_code is a segment selector when a descriptor-table reference raised
// the fault, and 0 otherwise. In long mode 0 is the common case and almost
// always means a non-canonical memory reference - typically a bad stack
// pointer, since a push to a non-canonical address faults here rather than
// producing a page fault.
void general_protection_handler(uint64_t error_code, uint64_t fault_rip, uint64_t fault_cs){

    printk("[#GP] general protection fault\n");
    printk("[#GP]  rip=0x%llx cs=0x%llx cpl=%d\n",
           (unsigned long long)fault_rip,
           (unsigned long long)fault_cs,
           (int)(fault_cs & 3));
    printk("[#GP]  error_code=0x%llx\n", (unsigned long long)error_code);

    if(error_code == 0){

        printk("[#GP]  no selector - non-canonical address or illegal operation\n");

    }else{

        // Selector error code: bit 0 external, bits 2:1 table, bits 15:3 index.
        const char *table;

        switch((error_code >> 1) & 0x3){
            case 0:  table = "GDT";  break;
            case 1:  table = "IDT";  break;
            case 2:  table = "LDT";  break;
            default: table = "IDT";  break;
        }

        printk("[#GP]  selector table=%s index=%d external=%d\n",
               table,
               (int)((error_code >> 3) & 0x1FFF),
               (int)(error_code & 1));
    }

    //kill process once ring3 faults are recoverable
    panic("general protection fault");
}

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
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); panic(msg); }
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

        msg = "kernel write hit a page that was not present error";

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
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); panic(msg); }
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


        msg = "user read hit a page that was not present error";

        

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
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); panic(msg); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); panic(msg); }


    }




    if(err_code == 0x5){

        //a user read hit a page that was present but violated protection.

        msg = "a user read hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a user read hit a page that was present but violated protection NX CASE.";

        }

        //kill process 
        panic(msg);


    }







       if(err_code == 0x6){

        //a user write hit a page that was not present.



        msg = "user write hit a page that was not present error";

        

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
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); panic(msg); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); panic(msg); }


    }








       if(err_code == 0x7){

        //a user write hit a page that was present but violated protection

        msg = "a user write hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a user write hit a page that was present but violated protection NX CASE.";

        }

        //kill proccess
        panic(msg);


    }
}

void keyboard_interrupt_handler(int irq, void *data) {
    (void)irq;
    (void)data;

    uint8_t scancode = inb(0x60);
    int got_char = keyboard_handle_scancode(scancode);
    pic_send_eoi(PIC_KEYBOARD_IRQ);

    if (got_char) {
        scheduler_cpu *cpu = scheduler_this_cpu();
        if (cpu != NULL) {
            scheduler_wake_one(cpu->cpu_id, cpu->keyboard_wait_queue);
        }
    }
}

void mouse_interrupt_handler(int irq, void *data) {
    (void)irq;
    (void)data;

    (void)inb(0x60);
    pic_send_eoi(PIC_MOUSE_IRQ);
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

