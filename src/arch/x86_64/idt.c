#include <stdint.h>
#include <stddef.h>
#include <idt.h>
#include <handlers.h>
#include <pic.h>
#include <syscall.h>

extern void page_fault_isr(void);
extern void keyboard_isr(void);
extern void mouse_isr(void);
extern void timer_isr(void);
extern void yield_isr(void);
extern void exit_isr(void);
extern void syscall_isr(void);

static inline void load_idt(struct idt_ptr *idtp);





struct idt_entry idt[256];

struct idt_ptr idtp;

void idt_init(){

idtp.limit = sizeof(struct idt_entry) * 256 - 1;
idtp.base  = (uint64_t)&idt;

set_idt_entry(14, (uintptr_t)page_fault_isr, 0x8E);
set_idt_entry(PIC_TIMER_VECTOR, (uintptr_t)timer_isr, 0x8E);
set_idt_entry(PIC_KEYBOARD_VECTOR, (uintptr_t)keyboard_isr, 0x8E);
set_idt_entry(PIC_MOUSE_VECTOR, (uintptr_t)mouse_isr, 0x8E);
set_idt_entry(YIELD_VECTOR, (uintptr_t)yield_isr, 0x8E);
set_idt_entry(EXIT_VECTOR, (uintptr_t)exit_isr, 0x8E);
// DPL=3 (0xEE, vs 0x8E for the ring0-only gates above): ring3 `int 0x80` must
// be allowed to reach this gate, otherwise the CPU raises #GP before the ISR
// ever runs.
set_idt_entry(SYSCALL_VECTOR, (uintptr_t)syscall_isr, 0xEE);

load_idt(&idtp);

}


static inline void load_idt(struct idt_ptr *idtp) {
    __asm__ volatile ("lidt %0" : : "m"(*idtp));
}


void set_idt_entry(int vector, uintptr_t handler, uint8_t type_attr){

    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF; 
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector = 0x08;
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].zero = 0;



}






