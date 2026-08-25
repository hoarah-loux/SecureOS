/*
 * BADPTR.EXE - what a hostile or broken ring3 program can do to the kernel.
 *
 * Two separate protections are under test here, and the second one kills this
 * program on purpose, so the test is only complete if the *next* command in
 * the autorun list runs afterwards. A machine that survives BADPTR.EXE and
 * still reaches the shell prompt is the passing result.
 *
 * Steps 1 to 4: every syscall that takes a pointer must reject one that names
 * kernel memory, rather than reading or writing it. 0xFFFFFFFF80000000 is
 * where the kernel image is mapped.
 *
 * Step 5: dereferencing an unmapped address must kill this process and
 * nothing else. Before the fault handler learned to do that, this line halted
 * the whole kernel and cost a reboot.
 */

#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "secureos_fb.h"
#include "secureos_syscall.h"

#define KERNEL_ADDR 0xFFFFFFFF80000000ULL

int main(void)
{
    int failures = 0;

    /* write() from a kernel address would print kernel memory to the console
     * and the serial line. */
    long rc = __sys3(SYS_write, 1, (void *)KERNEL_ADDR, 16);
    printf("[BADPTR] step 1: write(1, kernel_ptr, 16) = %ld\n", rc);
    if (rc >= 0) { printf("[BADPTR] FAIL: kernel pointer accepted by write\n"); failures++; }

    /* read() into a kernel address would have the FAT32 layer write file
     * contents over the kernel image. */
    int fd = open("/README.TXT", 0);
    if (fd < 0) {
        printf("[BADPTR] FAIL: cannot open /README.TXT\n");
        failures++;
    } else {
        rc = __sys3(SYS_read, fd, (void *)KERNEL_ADDR, 16);
        printf("[BADPTR] step 2: read(fd, kernel_ptr, 16) = %ld\n", rc);
        if (rc >= 0) { printf("[BADPTR] FAIL: kernel pointer accepted by read\n"); failures++; }
        close(fd);
    }

    /* open() has to scan the path string before it can use it. */
    rc = __sys3(SYS_open, (void *)KERNEL_ADDR, 0, 0);
    printf("[BADPTR] step 3: open(kernel_ptr) = %ld\n", rc);
    if (rc >= 0) { printf("[BADPTR] FAIL: kernel pointer accepted by open\n"); failures++; }

    /* And the framebuffer, which reads a whole screen's worth. */
    rc = secureos_fb_present((const uint32_t *)KERNEL_ADDR, 320, 200);
    printf("[BADPTR] step 4: fb_present(kernel_ptr) = %ld\n", rc);
    if (rc >= 0) { printf("[BADPTR] FAIL: kernel pointer accepted by fb_present\n"); failures++; }

    /* A valid user pointer must still work, or the checks are just breaking
     * things rather than guarding them. */
    char buf[32];
    fd = open("/README.TXT", 0);
    rc = (fd < 0) ? -1 : (long)read(fd, buf, sizeof(buf));
    printf("[BADPTR] step 5: read into a real buffer = %ld\n", rc);
    if (rc <= 0) { printf("[BADPTR] FAIL: valid pointer rejected\n"); failures++; }
    if (fd >= 0) { close(fd); }

    if (failures != 0) {
        printf("[BADPTR] %d checks failed\n", failures);
        return 1;
    }

    printf("[BADPTR] step 6: all kernel pointers rejected\n");
    printf("[BADPTR] step 7: faulting on purpose, this process should die\n");

    /* Deliberate. The kernel must kill this process and carry on: the next
     * autorun command running is the evidence. */
    volatile int *nowhere = (volatile int *)0x10;
    *nowhere = 1;

    printf("[BADPTR] FAIL: still alive after writing to an unmapped address\n");
    return 1;
}
