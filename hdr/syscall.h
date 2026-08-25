#pragma once
#include <stdint.h>
#include <context.h>
#include <syscall_abi.h>

// The ABI itself - SYSCALL_VECTOR and enum syscall_number - lives in
// syscall_abi.h so userland can share it without dragging in trap_frame.

typedef int64_t (*syscall_fn)(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

void syscall_handler(trap_frame *frame, trap_reason trap_reason);
