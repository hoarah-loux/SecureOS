#pragma once
#include <stdint.h>
#include <stddef.h>
#include <context.h>


void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip);
void keyboard_interrupt_handler(int irq, void *data);



void timer_handler(trap_frame *frame, trap_reason trap_reason);
uint64_t timer_ticks_get(void);


void yield_handler(trap_frame *frame, trap_reason trap_reason);

 