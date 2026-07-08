#pragma once
#include <stdint.h>
#include <stddef.h>
#include "idt.h"


void page_fault_handler(uint64_t faulting_va, uint64_t error_code, uint64_t fault_rip);

void break_point_handler();

 