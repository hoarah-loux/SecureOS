/* src/userland/apps/echoin/echoin.c
 *
 * Reading the keyboard from ring3.
 *
 * Three things are under test here and they fail differently, which is why
 * this reads twice by two different routes:
 *
 *   1. read(0, ...) blocks instead of spinning. Before SYS_read learned to
 *      return SYS_WOULD_BLOCK it returned 0 immediately, so a program waiting
 *      for input burned a whole CPU and newlib's stdio never made progress. A
 *      regression here shows up as this program pinning the machine, not as
 *      wrong output.
 *   2. keystrokes reach the foreground process rather than the shell. The
 *      shell is blocked in SYS_getkey_wait the whole time this runs; if input
 *      ownership is broken, the characters typed here land in the shell's line
 *      buffer instead and this program hangs forever.
 *   3. newlib's stdio works on top of both, so fgets() on stdin behaves.
 *
 * Nothing echoes typed characters - the kernel deliberately does not, so that
 * a program drawing to the framebuffer does not get keystrokes painted over
 * it. Echoing is the program's job, and this one does it so a screenshot shows
 * what was typed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    char line[128];
    size_t len = 0;

    printf("[ECHOIN] type a line, then Enter:\n");
    fflush(stdout);

    /* Route one: the raw syscall, one byte at a time. */
    for (;;) {
        char c;
        int n = read(STDIN_FILENO, &c, 1);

        if (n < 0) {
            printf("\n[ECHOIN] read failed\n");
            return 1;
        }
        if (n == 0) {
            continue;   /* short read; not EOF, this fd never ends */
        }

        if (c == '\n' || c == '\r') {
            break;
        }

        if (c == '\b') {
            if (len > 0) { len--; putchar('\b'); fflush(stdout); }
            continue;
        }

        if (len < sizeof(line) - 1) {
            line[len++] = c;
            putchar(c);          /* the kernel does not echo; we do */
            fflush(stdout);
        }
    }

    line[len] = '\0';
    printf("\n[ECHOIN] read() got %u bytes: \"%s\"\n", (unsigned)len, line);

    /* Route two: newlib stdio on the same fd. */
    printf("[ECHOIN] now one more line through fgets:\n");
    fflush(stdout);

    char buf[128];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        printf("[ECHOIN] fgets failed\n");
        return 1;
    }

    buf[strcspn(buf, "\r\n")] = '\0';
    printf("[ECHOIN] fgets got: \"%s\"\n", buf);

    printf("[ECHOIN] done, exiting 0\n");
    return 0;
}
