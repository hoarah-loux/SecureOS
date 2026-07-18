#pragma once
#include <stdint.h>
#include <stddef.h>


void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip);
void keyboard_interrupt_handler(int irq, void *data);

void break_point_handler();
void timer_handler();
uint64_t timer_ticks_get(void);

 