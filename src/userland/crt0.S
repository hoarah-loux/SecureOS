.intel_syntax noprefix

/* C runtime startup for SecureOS userland binaries.
 *
 * create_thread() (src/kernel/proc/ds/t.c) hands a new ring3 thread a zeroed
 * register file and an rsp pointing at the top of its user stack. It puts
 * nothing on that stack: no argc, no argv, no envp, no auxv. So unlike a Linux
 * crt0 there is nothing to unpack here - we synthesise the arguments instead.
 */

.section .text.startup
.global _start
.type _start, @function
_start:
    /* End the frame-pointer chain so a backtrace stops here rather than
     * wandering into whatever the stack page happened to contain. */
    xor rbp, rbp

    /* The SysV ABI wants rsp 16-byte aligned at a function's entry, i.e.
     * aligned *before* the call pushes the return address. create_thread sets
     * rsp to the region end, which is page-aligned, but align defensively
     * rather than depend on that. */
    and rsp, -16

    /* Zero .bss.
     *
     * Redundant as of the p_filesz clamp in populate_fault_frame(): the kernel
     * now zero-fills everything past p_filesz, so .bss arrives zeroed. Kept
     * anyway, because every real crt0 does it and because the C standard makes
     * it this code's guarantee rather than the loader's - a binary that
     * depends on the kernel getting it right has no way to notice when it does
     * not. The cost is touching pages that would otherwise fault in lazily. */
    lea rdi, [rip + __bss_start]
    lea rcx, [rip + __bss_end]
    sub rcx, rdi
    xor eax, eax
    cld
    rep stosb

    /* Run static constructors and register the destructor pass. Provided by
     * newlib; the freestanding smoke-test build supplies its own no-op. */
    call __libc_init_array

    /* main(argc, argv, envp) */
    mov edi, 1
    lea rsi, [rip + __user_argv]
    lea rdx, [rip + __user_envp]
    call main

    /* exit(), not _exit(): stdout is line/block buffered and the pending
     * buffer has to be flushed before the process goes away. exit() runs the
     * atexit chain, which is where newlib flushes, then calls _exit(), which
     * is our SYS_exit wrapper. */
    mov edi, eax
    call exit

    /* exit() does not return. If it somehow does, do not fall into whatever
     * follows in .text. */
1:  hlt
    jmp 1b
.size _start, . - _start

.section .rodata
__user_progname:
    .asciz "prog"

.section .data
.align 8
__user_argv:
    .quad __user_progname
    .quad 0
__user_envp:
    .quad 0
