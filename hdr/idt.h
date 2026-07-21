#pragma once
#include <stdint.h>
#include <stddef.h>


#define YIELD_VECTOR 0x81
#define EXIT_VECTOR  0x82




typedef struct __attribute__((packed)) idt_entry{
    
    uint16_t offset_low;      // bits 0–15 of handler address
    uint16_t selector;        // code segment selector (from GDT)
    uint8_t  ist;             // interrupt stack table index (usually 0)
    uint8_t  type_attr;       // flags: present, DPL, gate type
    uint16_t offset_mid;      // bits 16–31 of handler address
    uint32_t offset_high;     // bits 32–63 of handler address
    uint32_t zero;            // reserved, must be 
}idt_entry;


typedef struct __attribute__((packed)) idt_ptr {

    uint16_t limit;
    uint64_t base;
    
}idt_ptr;

void idt_init(void);
void set_idt_entry(int vector, uintptr_t handler, uint8_t type_attr);

