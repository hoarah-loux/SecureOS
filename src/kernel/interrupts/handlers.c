#include <vmm.h>
#include <pmm.h>
#include <pic.h>
#include <keyboard.h>
#include <handlers.h>
#include <serial.h>
#include <fs_syscalls.h>
#include <idt.h>
#include <scheduler.h>

// EXIT_VECTOR is a number in a header and `int` wants it spelled into the
// asm text, so it has to survive one round of macro expansion first. Same
// trick, same reason, as in src/kernel/interrupts/syscall.c.
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)


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

    if (region->object != NULL &&
        region->object->type == FILE &&
        region->object->backing_ctx != NULL) {

        if (alligned_va < region->va_start) {
            return 0;
        }

        uint64_t page_delta = (uint64_t)(alligned_va - region->va_start);
        uint64_t file_offset = (uint64_t)region->object_base_offset + page_delta;
        if (file_offset < page_delta) {
            return 0;
        }

        // Only file_valid_len bytes of this region come from the file; the
        // rest is .bss and must read as zero. Reading a full page regardless
        // is what used to hand a process the bytes that happen to follow its
        // segment on disk - typically the symbol table, whose first entry is
        // all zeroes, which is exactly enough to make a one-qword test pass
        // and a real program fail later.
        //
        // Three cases, in order: the page is entirely past the file content
        // and is pure zero fill; the page straddles the boundary and is part
        // read, part zeroed; the page is entirely file content.
        uint64_t valid = 0;
        if (page_delta < (uint64_t)region->file_valid_len) {
            valid = (uint64_t)region->file_valid_len - page_delta;
            if (valid > 4096) { valid = 4096; }
        }

        if (valid > 0 &&
            fs_mmap_read(region->object->backing_ctx, file_offset, frame_buf,
                         valid) < 0) {
            return 0;
        }

        // Zeroing the tail is not optional even on a fully-file-backed page:
        // the frame comes from the physical allocator carrying whatever the
        // last owner left in it, so a short read would otherwise leak it.
        for (uint64_t i = valid; i < 4096; i++) {
            frame_buf[i] = 0;
        }

        return 1;
    }

    for (uint64_t i = 0; i < 4096; i++) {
        frame_buf[i] = 0;
    }

    return 1;
}

// #GP, vector 13.
//
// Until this existed vector 13 had no gate at all, so every #GP escalated
// straight to #DF and then triple fault - a silent reboot with nothing on the
// serial line. Nothing can safely resume a thread that executed an illegal
// operation, so there is no recovery in the sense of continuing - but a ring3
// fault is survivable by killing the process, which is what this now does.
//
// error_code is a segment selector when a descriptor-table reference raised
// the fault, and 0 otherwise. In long mode 0 is the common case and almost
// always means a non-canonical memory reference - typically a bad stack
// pointer, since a push to a non-canonical address faults here rather than
// producing a page fault.
static void kill_current_process(const char *reason);

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

    // A #GP raised at CPL 3 is a bug in the user program - a non-canonical
    // address, usually from a wrecked stack pointer, or an instruction ring3
    // is not allowed to execute. The kernel was not in the middle of anything
    // when it happened, so the process dies and the machine does not.
    //
    // At CPL 0 it is the kernel itself that did something illegal, and there
    // is nothing to hand the failure to. That stays a halt.
    if((fault_cs & 3) == 3){
        printk("[#GP]  raised in user mode, killing process\n");
        kill_current_process("user #GP: nothing else runnable, cannot kill the faulting process");
    }

    panic("general protection fault");
}

// "not present" on its own says nothing about what went wrong. Dump the
// address, the faulting instruction, the raw error code and which step gave
// up, for both the fatal and the survivable case.
static void pf_report(const char *what, const char *msg, const char *stage,
                      uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip){

    printk("[#PF] %s\n", what);
    printk("[#PF]  va=0x%llx rip=0x%llx error_code=0x%llx\n",
           (unsigned long long)faulting_va,
           (unsigned long long)fault_rip,
           (unsigned long long)error_code);
    printk("[#PF]  present=%d write=%d user=%d reserved=%d ifetch=%d\n",
           (int)((error_code >> 0) & 1),
           (int)((error_code >> 1) & 1),
           (int)((error_code >> 2) & 1),
           (int)((error_code >> 3) & 1),
           (int)((error_code >> 4) & 1));
    printk("[#PF]  gave up at: %s\n", stage);
    printk("[#PF]  %s\n", msg);
}

// A page fault the handler cannot service, taken in kernel mode, is fatal.
// There is no way to unwind a kernel stack that may be holding a half-finished
// allocation or a device mid-transfer, so this stays a halt.
static void pf_panic(const char *msg, const char *stage, uint64_t faulting_va,
                     uint64_t error_code, uint64_t fault_rip){

    pf_report("unhandled page fault", msg, stage, faulting_va, error_code, fault_rip);
    panic(msg);
}

// The same fault taken in user mode kills the process instead of the machine.
//
// A ring3 program that dereferences garbage is a bug in that program, and the
// kernel has everything it needs to survive it: the faulting thread is not
// holding kernel state, because the fault happened while user code was
// running, and the teardown behind EXIT_VECTOR already returns the keyboard,
// frees the address space and picks something else to run. Before this, a
// null dereference in a userland program halted the whole kernel and cost a
// reboot - which during doom bring-up was the single most expensive thing in
// the edit-test loop.
//
// The exit trap is taken from CPL0, so it is a same-privilege trap: no stack
// switch, the frame nests on the dying thread's kernel stack, and that stack
// is abandoned with the thread. This is the same mechanism sys_exit() uses.
//
// It comes back only when exit_handler() bails, which it does when
// scheduler_pick_next() finds nothing else runnable. There is no honest way to
// continue from there - the current thread has to die and nothing can replace
// it - so that case falls through to the panic.
#define FAULT_EXIT_STATUS 139   /* 128 + SIGSEGV, the shell convention */

static void kill_current_process(const char *reason){

    scheduler_cpu *cpu = scheduler_this_cpu();
    if(cpu == NULL || cpu->current_thread == NULL){
        panic("fault in user mode with no current thread to kill");
    }

    cpu->current_thread->exit_status = FAULT_EXIT_STATUS;

    __asm__ volatile("int $" STRINGIFY(EXIT_VECTOR) ::: "memory");

    panic(reason);
}

static void pf_kill_current(const char *msg, const char *stage, uint64_t faulting_va,
                            uint64_t error_code, uint64_t fault_rip){

    pf_report("fault in user mode, killing process", msg, stage,
              faulting_va, error_code, fault_rip);

    kill_current_process("user page fault: nothing else runnable, cannot kill the faulting process");
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
        if(this_cpu == NULL){ pf_panic(msg, "no scheduler_cpu", faulting_va, error_code, fault_rip); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ pf_panic(msg, "no current thread", faulting_va, error_code, fault_rip); }
        if(curr_t->owning_proc == NULL){ pf_panic(msg, "thread has no owning proc", faulting_va, error_code, fault_rip); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ pf_panic(msg, "proc has no address space", faulting_va, error_code, fault_rip); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ pf_panic(msg, "va is in no vm_region of this space", faulting_va, error_code, fault_rip); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ pf_panic(msg, "pmm_alloc_frame failed", faulting_va, error_code, fault_rip); }
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); pf_panic(msg, "populate_fault_frame failed", faulting_va, error_code, fault_rip); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); pf_panic(msg, "vmm_map_page failed", faulting_va, error_code, fault_rip); }
        

    }






    if(err_code == 0x1){

        //a kernel read hit a page that was present but violated protection.
        msg = "a kernel read hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a kernel read hit a page that was present but violated protection NX CASE.";

        }
        pf_panic(msg, "protection violation, not recoverable", faulting_va, error_code, fault_rip);
        
    }







    if(err_code == 0x2){

        // this code is a duplicate of the first case but we need to isolate for future additions
        // just to be safe 

        msg = "kernel write hit a page that was not present error";

        //a kernel write hit a page that was not present.

         uintptr_t alligned_va = faulting_va & ~0xFFFULL;
        //get curr active space from scvheduelr
        scheduler_cpu *this_cpu = scheduler_this_cpu();
        if(this_cpu == NULL){ pf_panic(msg, "no scheduler_cpu", faulting_va, error_code, fault_rip); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ pf_panic(msg, "no current thread", faulting_va, error_code, fault_rip); }
        if(curr_t->owning_proc == NULL){ pf_panic(msg, "thread has no owning proc", faulting_va, error_code, fault_rip); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ pf_panic(msg, "proc has no address space", faulting_va, error_code, fault_rip); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ pf_panic(msg, "va is in no vm_region of this space", faulting_va, error_code, fault_rip); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ pf_panic(msg, "pmm_alloc_frame failed", faulting_va, error_code, fault_rip); }
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); pf_panic(msg, "populate_fault_frame failed", faulting_va, error_code, fault_rip); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); pf_panic(msg, "vmm_map_page failed", faulting_va, error_code, fault_rip); }

    }






    if(err_code == 0x3){

        //a kernel write hit a page that was present but violated protection.

          
        msg = "a kernel write hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a kernel write hit a page that was present but violated protection NX CASE.";

        }
        pf_panic(msg, "protection violation, not recoverable", faulting_va, error_code, fault_rip);

    }






    if(err_code == 0x4){

        //a user read hit a page that was not present.


        msg = "user read hit a page that was not present error";

        

         uintptr_t alligned_va = faulting_va & ~0xFFFULL;
        //get curr active space from scvheduelr
        scheduler_cpu *this_cpu = scheduler_this_cpu();
        if(this_cpu == NULL){ pf_kill_current(msg, "no scheduler_cpu", faulting_va, error_code, fault_rip); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ pf_kill_current(msg, "no current thread", faulting_va, error_code, fault_rip); }
        if(curr_t->owning_proc == NULL){ pf_kill_current(msg, "thread has no owning proc", faulting_va, error_code, fault_rip); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ pf_kill_current(msg, "proc has no address space", faulting_va, error_code, fault_rip); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ pf_kill_current(msg, "va is in no vm_region of this space", faulting_va, error_code, fault_rip); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ pf_kill_current(msg, "pmm_alloc_frame failed", faulting_va, error_code, fault_rip); }
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); pf_kill_current(msg, "populate_fault_frame failed", faulting_va, error_code, fault_rip); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); pf_kill_current(msg, "vmm_map_page failed", faulting_va, error_code, fault_rip); }


    }




    if(err_code == 0x5){

        //a user read hit a page that was present but violated protection.

        msg = "a user read hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a user read hit a page that was present but violated protection NX CASE.";

        }

        //kill process 
        pf_kill_current(msg, "protection violation, not recoverable", faulting_va, error_code, fault_rip);


    }







       if(err_code == 0x6){

        //a user write hit a page that was not present.



        msg = "user write hit a page that was not present error";

        

         uintptr_t alligned_va = faulting_va & ~0xFFFULL;
        //get curr active space from scvheduelr
        scheduler_cpu *this_cpu = scheduler_this_cpu();
        if(this_cpu == NULL){ pf_kill_current(msg, "no scheduler_cpu", faulting_va, error_code, fault_rip); }
        thread *curr_t = scheduler_current(this_cpu->cpu_id);
        if(curr_t == NULL){ pf_kill_current(msg, "no current thread", faulting_va, error_code, fault_rip); }
        if(curr_t->owning_proc == NULL){ pf_kill_current(msg, "thread has no owning proc", faulting_va, error_code, fault_rip); }
        vm_space *this_space = curr_t->owning_proc->adress_space;
        if(this_space == NULL){ pf_kill_current(msg, "proc has no address space", faulting_va, error_code, fault_rip); }
        uintptr_t *this_space_root_ptr = this_space->pml4_phys_ptr;
        search_region_list_out search_md = search_region_list(this_space, faulting_va);
        if(!search_md.success){ pf_kill_current(msg, "va is in no vm_region of this space", faulting_va, error_code, fault_rip); }
        vm_region *this_region = search_md.region;
        uint64_t perms = this_region->permissions; // have to add later (is this region allowed to lazy populate)
        uintptr_t physical_frame = pmm_alloc_frame();
        if(!physical_frame){ pf_kill_current(msg, "pmm_alloc_frame failed", faulting_va, error_code, fault_rip); }
        if(!populate_fault_frame(this_region, alligned_va, physical_frame)){ pmm_free_frame(physical_frame); pf_kill_current(msg, "populate_fault_frame failed", faulting_va, error_code, fault_rip); }
        map_out map_md = vmm_map_page(alligned_va, physical_frame, this_space_root_ptr, 4, perms); // finally im calling this function
        if(!map_md.success){ pmm_free_frame(physical_frame); pf_kill_current(msg, "vmm_map_page failed", faulting_va, error_code, fault_rip); }


    }








       if(err_code == 0x7){

        //a user write hit a page that was present but violated protection

        msg = "a user write hit a page that was present but violated protection.";

        if(error_code & PFEC_ID){//nx case

            msg = "a user write hit a page that was present but violated protection NX CASE.";

        }

        //kill proccess
        pf_kill_current(msg, "protection violation, not recoverable", faulting_va, error_code, fault_rip);


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
            scheduler_wake_all(cpu->cpu_id, cpu->keyboard_wait_queue);
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

    // i8042 poll fallback for machines where firmware USB legacy emulation
    // presents ports 60h/64h but never raises IRQ1 once the OS owns the
    // machine (observed on the ASUS H670 target: keyboard alive in the
    // bootloader, silent in the kernel). Feeds the exact same path as the
    // IRQ1 handler; whichever fires first consumes the byte, so both being
    // active is harmless. Guards: 0xFF status is a floating bus (no
    // controller at all), bit 5 marks mouse (AUX) data - skip both.
    {
        uint8_t kb_status = inb(0x64);
        if (kb_status != 0xFF && (kb_status & 0x01) != 0) {
            uint8_t kb_scancode = inb(0x60);
            // Bytes flagged as mouse data (bit 5) or arriving with the
            // timeout/parity error bits (6/7) are consumed to unblock the
            // controller but never fed to the scancode decoder.
            if ((kb_status & 0xE0) == 0 &&
                keyboard_handle_scancode(kb_scancode)) {
                scheduler_wake_all(cpu->cpu_id, cpu->keyboard_wait_queue);
            }
        }
    }

    // Anything whose SYS_sleep deadline has passed goes back on the ready
    // queue. Done before scheduler_tick() so a thread that became runnable on
    // this very tick is a candidate for the reschedule below rather than
    // waiting a further quantum.
    (void)scheduler_wake_expired_sleepers(cpu->cpu_id, timer_ticks);
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

