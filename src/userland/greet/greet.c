/* src/userland/greet/greet.c
 *
 * The worked example from ../README.md, kept in the tree so the workflow that
 * document describes is one that demonstrably builds and runs rather than one
 * that merely looks right. If the instructions there stop working, this stops
 * building.
 *
 * Output goes to the serial port, not the console: sys_write sends fd 1 to
 * serial_write_char. Watch serial.log.
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    char *buf = malloc(1024);
    if (!buf) { printf("[GREET] malloc failed\n"); return 1; }

    snprintf(buf, 1024, "hello from ring3");
    printf("[GREET] %s\n", buf);

    free(buf);
    return 0;
}
