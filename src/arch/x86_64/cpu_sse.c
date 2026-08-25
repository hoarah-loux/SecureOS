#include <cpu_sse.h>

// Turning SSE on.
//
// The kernel is built -mno-mmx -mno-sse -mno-sse2 and never executes a vector
// instruction, so on its own account it would not need any of this. Ring3 is a
// different matter: SSE is not an optional extension in the x86-64 SysV ABI.
// Floating-point arguments are passed in xmm0-xmm7, a varargs call sets al to
// the number of vector registers used, and gcc lowers ordinary struct
// assignments and string copies to movdqa/movdqu at any optimisation level. A
// C program compiled against a normal libc executes SSE within the first few
// instructions of crt0 whether or not it mentions a float.
//
// Out of reset the CPU is configured to fault on all of that: CR0.EM makes
// vector instructions raise #UD, and CR4.OSFXSR being clear makes FXSAVE and
// FXRSTOR raise #UD too - which the context switch needs, not just userspace.
//
// The four bits, and why each one:
//
//   CR0.EM  (bit 2, "emulation") cleared. Set, it routes SSE and x87 to #UD so
//           software can emulate them. There is no emulator here, so it must
//           be off before anything executes a vector instruction.
//   CR0.MP  (bit 1, "monitor coprocessor") set. Governs how WAIT/FWAIT
//           behaves with respect to CR0.TS. Intel specifies MP=1, EM=0 for a
//           system with a real FPU.
//   CR4.OSFXSR (bit 9) set. This is the OS telling the CPU two things at once:
//           that SSE instructions are permitted, and that the OS knows how to
//           save and restore the register file with FXSAVE/FXRSTOR. Both are
//           true only once the context switch actually does so, which is why
//           this and the fxsave/fxrstor pair land together.
//   CR4.OSXMMEXCPT (bit 10) set. Directs unmasked SSE numeric exceptions to
//           #XM (vector 19) instead of #UD, so a floating-point error in a
//           user program is at least reported as what it is.
//
// CR0.TS is deliberately left alone. Setting it is how lazy FPU switching
// arranges a #NM on the first vector instruction after a context switch, and
// this kernel saves eagerly instead - see thread::fpu_state.

// CPUID leaf 1, EDX feature bits.
#define CPUID_1_EDX_FPU  (1u << 0)
#define CPUID_1_EDX_FXSR (1u << 24)
#define CPUID_1_EDX_SSE  (1u << 25)
#define CPUID_1_EDX_SSE2 (1u << 26)

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)

#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)

bool cpu_enable_sse(void) {

    uint32_t eax, ebx, ecx, edx;

    // Long mode implies CPUID exists, so there is no need for the EFLAGS.ID
    // dance a 32-bit kernel would do here.
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));

    // FXSR is checked alongside SSE because the context switch depends on
    // FXSAVE/FXRSTOR existing, not merely on the vector instructions.
    const uint32_t required = CPUID_1_EDX_FPU | CPUID_1_EDX_FXSR |
                              CPUID_1_EDX_SSE | CPUID_1_EDX_SSE2;

    if ((edx & required) != required) {
        return false;
    }

    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP;
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0) : "memory");

    uint64_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4) : "memory");

    // The x87 stack, tag word and control word come up in an unspecified state
    // and FXSAVE would otherwise write that out as thread 0's starting image.
    // FNINIT is the architectural reset for the x87 side; the SSE side is
    // covered by each thread's fpu_state being initialised at creation.
    __asm__ __volatile__("fninit");

    return true;
}
