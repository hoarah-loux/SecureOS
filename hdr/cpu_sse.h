#pragma once
#include <stdint.h>
#include <stdbool.h>

// Enables SSE for this CPU: clears CR0.EM, sets CR0.MP, CR4.OSFXSR and
// CR4.OSXMMEXCPT, and runs FNINIT.
//
// Must be called before the first context switch, not merely before the first
// user program: restore_context uses FXRSTOR, which raises #UD while
// CR4.OSFXSR is clear.
//
// Returns false and changes nothing if CPUID says the CPU lacks FPU, FXSR,
// SSE or SSE2. Every x86-64 CPU has all four - the check is there so a failure
// is a clear message rather than a #UD in the scheduler.
//
// See src/arch/x86_64/cpu_sse.c for why a kernel built -mno-sse needs this.
bool cpu_enable_sse(void);
