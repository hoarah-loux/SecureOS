// Milestone 1 of the doom port: the first C program this OS has ever run.
//
// Everything before this was hand-written assembly (elf_ring3_test.S and
// friends), which proved the loader and the syscall gate but not much else.
// This proves the parts a real program needs and the assembly tests never
// touched:
//
//   - a C toolchain and linker script producing a binary this loader accepts;
//   - crt0 reaching main with a usable stack and zeroed .bss;
//   - newlib linked and working, which means malloc, stdio and varargs;
//   - SSE enabled, since the x86-64 ABI passes varargs' vector-register count
//     in al and gcc emits xmm for ordinary struct and string work;
//   - an executable well over the old 4 KiB loader ceiling.
//
// Output goes to the serial port, not the console: sys_write sends fd 1 to
// serial_write_char (src/kernel/fs/fs_syscalls.c). Watch serial.log.
//
// Each step prints before and after doing the thing that could fail, so a
// hang or a fault localises to a step rather than to "it stopped".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    printf("[HELLO] step 1: printf works\n");

    printf("[HELLO] step 2: argc=%d argv[0]=%s\n",
           argc, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");

    // Exercises varargs across the integer and vector register classes, and
    // newlib's formatter. A broken SSE setup usually dies here rather than at
    // step 1, because %f is what actually reads xmm0.
    printf("[HELLO] step 3: %d %s %c 0x%lx %.3f\n",
           -42, "str", 'x', 0xdeadbeefUL, 3.14159);

    // malloc goes through _sbrk, which is SYS_brk: a cursor inside the 64 MiB
    // anonymous demand-paged region the loader reserves per process. Touch
    // every byte so a page that was reserved but never mapped faults here
    // rather than somewhere confusing later.
    size_t n = 64 * 1024;
    char *buf = malloc(n);
    if (!buf) {
        printf("[HELLO] step 4: FAIL malloc(%lu) returned NULL\n",
               (unsigned long)n);
        return 1;
    }
    memset(buf, 0xA5, n);

    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)buf[i] != 0xA5) { bad++; }
    }
    printf("[HELLO] step 4: malloc+memset %lu bytes, %lu mismatches\n",
           (unsigned long)n, (unsigned long)bad);
    free(buf);

    // .bss really is zero - now guaranteed twice over, by the p_filesz clamp
    // in populate_fault_frame() and by crt0 zeroing it as well.
    static char zeroed[8192];
    size_t nonzero = 0;
    for (size_t i = 0; i < sizeof(zeroed); i++) {
        if (zeroed[i] != 0) { nonzero++; }
    }
    printf("[HELLO] step 5: .bss %lu bytes, %lu nonzero\n",
           (unsigned long)sizeof(zeroed), (unsigned long)nonzero);

    // String and heap round trip through newlib rather than our own helpers.
    char *dup = strdup("secureos");
    printf("[HELLO] step 6: strdup=%s len=%lu\n",
           dup ? dup : "(null)", dup ? (unsigned long)strlen(dup) : 0UL);
    free(dup);

    printf("[HELLO] done, exiting 0\n");

    // Returning here goes back into crt0, which calls exit() rather than
    // _exit() so this last line is actually flushed before the process dies.
    return 0;
}
