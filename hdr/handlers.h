#pragma once
#include <stdint.h>
#include <stddef.h>
#include <context.h>
#include <scheduler.h>
// Higher bits
#define PFEC_RSVD     (1ULL << 3)  // reserved-bit violation
#define PFEC_ID       (1ULL << 4)  // instruction fetch
#define PFEC_PK       (1ULL << 5)  // protection key violation
#define PFEC_SS       (1ULL << 6)  // shadow stack
#define PFEC_SGX      (1ULL << 15) // SGX violation

void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip);
void keyboard_interrupt_handler(int irq, void *data);



void timer_handler(trap_frame *frame, trap_reason trap_reason);
uint64_t timer_ticks_get(void);


void yield_handler(trap_frame *frame, trap_reason trap_reason);

void exit_handler(trap_frame *frame, trap_reason trap_reason);

 