#pragma once

// Raw syscall entry for SecureOS userland.
//
// The ABI (hdr/syscall_abi.h): number in rax, arguments in rdi, rsi, rdx, r10,
// r8, r9, return value back in rax. Note r10 rather than rcx - that is the
// SYSCALL/SYSRET convention leaking into an int-gate ABI, and it costs nothing
// to keep, so the numbers here match what the kernel's syscall_isr expects.
//
// syscall_isr (src/arch/x86_64/interrupts.asm) pushes and pops the full
// general-purpose register set around the dispatch, so an int 0x80 clobbers
// nothing except rax. The "memory" clobber is still needed: the kernel reads
// and writes buffers we hand it, and the compiler has to know that.
//
// Every syscall returns a flat -1 on failure. There is no errno at the kernel
// boundary; syscalls.c invents plausible errno values on the way out.

#include <syscall_abi.h>

static inline long __secureos_syscall(long nr,
                                      long a0, long a1, long a2,
                                      long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (a0), "S" (a1), "d" (a2),
          "r" (r10), "r" (r8), "r" (r9)
        : "memory", "cc"
    );

    return ret;
}

#define __sys0(nr)                     __secureos_syscall((nr), 0, 0, 0, 0, 0, 0)
#define __sys1(nr, a)                  __secureos_syscall((nr), (long)(a), 0, 0, 0, 0, 0)
#define __sys2(nr, a, b)               __secureos_syscall((nr), (long)(a), (long)(b), 0, 0, 0, 0)
#define __sys3(nr, a, b, c)            __secureos_syscall((nr), (long)(a), (long)(b), (long)(c), 0, 0, 0)
