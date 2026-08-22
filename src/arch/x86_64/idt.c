#include <stdint.h>
#include <stddef.h>
#include <idt.h>
#include <handlers.h>
#include <pic.h>
#include <syscall.h>

#define PHYS_MAP_BASE 0xFFFF800000000000ULL //slot 256 in the pml4
#define HIGHER_HALF_BASE_ 0xFFFF800000000000ULL // slot 256 in the pml4
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000 //slot 511 in the pml4

extern void page_fault_isr(void);
extern void keyboard_isr(void);
extern void mouse_isr(void);
extern void timer_isr(void);
extern void yield_isr(void);
extern void exit_isr(void);
extern void syscall_isr(void);

static inline void load_idt(struct idt_ptr *idtp);


uintptr_t page_fault_isr_off = (uintptr_t)&page_fault_isr + KERNEL_IMAGE_BASE;
uintptr_t keyboard_isr_off = (uintptr_t)&keyboard_isr + KERNEL_IMAGE_BASE;
uintptr_t mouse_isr_off = (uintptr_t)&mouse_isr + KERNEL_IMAGE_BASE;
uintptr_t timer_isr_off = (uintptr_t)&timer_isr + KERNEL_IMAGE_BASE;
uintptr_t yield_isr_off = (uintptr_t)&yield_isr + KERNEL_IMAGE_BASE;
uintptr_t exit_isr_off = (uintptr_t)&exit_isr + KERNEL_IMAGE_BASE;
uintptr_t syscall_isr_off = (uintptr_t)&syscall_isr + KERNEL_IMAGE_BASE;






struct idt_entry idt[256];

struct idt_ptr idtp;

void idt_init(){

idtp.limit = sizeof(struct idt_entry) * 256 - 1;

// idt_init() runs from kmain_highhalf, at the kernel's higher-half alias.
// &idt and every ISR symbol's address below are now RIP-relative (the
// kernel is built with -fpie - see src/boot/Makefile), so they resolve to
// addresses in whichever alias this code is *currently* executing from,
// not a fixed link-time value - no manual phys_to_virt() translation
// needed here (that was only required back when the build used
// -mcmodel=kernel, which forces absolute addressing for data references).
idtp.base = (uint64_t)&idt;

set_idt_entry(14, page_fault_isr_off, 0x8E);
set_idt_entry(PIC_TIMER_VECTOR, timer_isr_off, 0x8E);
set_idt_entry(PIC_KEYBOARD_VECTOR, keyboard_isr_off, 0x8E);
set_idt_entry(PIC_MOUSE_VECTOR, mouse_isr_off, 0x8E);
// DPL=3 (0xEE, vs 0x8E for the ring0-only gates above): ring3 `int` must be
// allowed to reach these gates, otherwise the CPU raises #GP before the ISR
// ever runs. Yield/exit/syscall are all invoked by user threads.
set_idt_entry(YIELD_VECTOR, yield_isr_off, 0xEE);
set_idt_entry(EXIT_VECTOR, exit_isr_off, 0xEE);
set_idt_entry(SYSCALL_VECTOR, syscall_isr_off, 0xEE);

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






