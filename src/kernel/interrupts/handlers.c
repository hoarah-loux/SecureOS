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
}

uint64_t timer_ticks_get(void) {
    return timer_ticks;
}


void yield_handler(trap_frame *frame, trap_reason trap_reason){

    return;

}
