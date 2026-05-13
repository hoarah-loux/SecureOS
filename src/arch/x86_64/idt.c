#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "kernel/interrupts/handlers.h"





struct idt_entry idt[256];

struct idt_ptr idtp;

void idt_init(){

idtp.limit = sizeof(struct idt_entry) * 256 - 1;
idtp.base  = (uint64_t)&idt;

set_idt_entry(14, (uintptr_t)page_fault_handler);

load_idt(&idtp);

}


static inline void load_idt(struct idt_ptr *idtp) {
    __asm__ volatile ("lidt %0" : : "m"(*idtp));
}


void set_idt_entry(int vector, uintptr_t handler){

    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF; 
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector = 0x08;
    idt[vector].ist = 0;
    idt[vector].type_attr = 0x8E;
    idt[vector].zero = 0;



}






