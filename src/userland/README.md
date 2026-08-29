# Writing a C program for SecureOS

Userland programs are ordinary C, compiled with the host `gcc`, linked against
the vendored newlib in [`newlib/`](newlib/), and run from the shell with
`./NAME`. This is the whole workflow.

## 1. Write it

Make a directory under `src/userland/apps/` and write plain C. `printf`,
`malloc`, `strdup`, `fopen`, `sqrt` all work.

```sh
mkdir -p src/userland/apps/greet
```

```c
/* src/userland/apps/greet/greet.c */
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
```

Output goes to the **serial port**, not the framebuffer console: `sys_write`
sends fd 1 to `serial_write_char`. Watch `serial.log`.

## 2. There is no step 2

The Makefile discovers programs by scanning `src/userland/apps/`. Every
subdirectory is one program; every `.c` file directly inside it is compiled and
linked into that program, so a program may be split across several files.
Nothing needs to be registered anywhere.

The name on the disk image is the directory name uppercased and truncated to
eight characters, since FAT 8.3 short names are all the shell's path lookup
understands. Dashes become underscores.

| Source | Runs as |
|---|---|
| `apps/greet/greet.c` | `./GREET.EXE` |
| `apps/calc/{calc.c,ops.c}` | `./CALC.EXE` |
| `apps/framebuffer-test/*.c` | `./FRAMEBU.EXE` |

That last row is why long names are a bad idea: pick something that is still
recognisable after eight characters. Two programs whose names collide once
truncated are a hard build error rather than one silently overwriting the other.

Subdirectories below an app directory are not scanned — put headers and data
there, not sources you expect to be compiled.

## 3. Build and run it

```sh
# From the project root, where the Makefile lives.

# Build the kernel, with the shell set to run your program at startup.
touch src/kernel/core/kernel.c         # make does not track CFLAGS changes
make CFLAGS_EXTRA='-DSHELL_AUTORUN="\"./GREET.EXE\""'

# Build the disk image. The absolute path is required - see below.
make "$PWD/src/boot/build/secureos.fat32.img"

# Boot headless; program output lands in serial.log.
qemu-system-x86_64 -machine pc \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=src/boot/build/OVMF_VARS_4M.fd \
  -boot order=d -cdrom secureos-uefi.iso \
  -drive file=src/boot/build/secureos.fat32.img,format=raw,if=ide,index=0,media=disk \
  -serial file:serial.log -display none -m 2048 -no-reboot

tail -20 serial.log
```

For an interactive run, `make run-uefi-serial` builds userland, the image and
the ISO and boots to the shell with the serial console on your terminal; type
`./GREET.EXE` there. `make userland` builds the programs alone and prints the
names they will have on the image, which is the quickest way to check that a new
directory was picked up.

`SHELL_AUTORUN` accepts several commands separated by `;`, which is how two
programs are started at once:

```sh
make CFLAGS_EXTRA='-DSHELL_AUTORUN="\"./RING3F.EXE;./RING3G.EXE\""'
```

## Four things that will waste your time

**make does not track `CFLAGS`.** Changing only `SHELL_AUTORUN` rebuilds
nothing and you boot the previous kernel. `touch src/kernel/core/kernel.c`
first. This looks exactly like your program not running. Headers *are* tracked
now (`-MMD -MP`), so editing one does rebuild everything that includes it —
only command-line defines are invisible to make.

**The FAT32 image needs its absolute path.** `$(FAT32_IMAGE)` expands under an
absolute `BUILD_DIR`, so `make build/secureos.fat32.img` matches no rule. It
is also not a prerequisite of `all`, so a plain `make` leaves a stale image
with your old binary on it.

**`build/OVMF_VARS_4M.fd` must exist and be writable.** Copy it once:

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd src/boot/build/OVMF_VARS_4M.fd
chmod u+w src/boot/build/OVMF_VARS_4M.fd
```

**Print your way through.** There is no debugger on the guest. A program that
prints a marker before each stage localises a hang or a fault to a stage; one
that prints only at the end tells you nothing. Add `-d int,cpu_reset -D
qemu.log` and a fault gives you RIP, RSP, CR2 and CR3 — resolve a kernel RIP
with `nm -n src/boot/build/secureos.elf` and subtract `0xFFFFFFFF80000000`.

## What you get

Working: `printf`/`snprintf` and the rest of stdio, `malloc`/`free`/`realloc`,
the string and memory functions, `strdup`, math from `libm`, `open`/`read`/
`write`/`close`/`lseek` on FAT32 files, `gettimeofday`, floating point
including varargs `%f`, static constructors, and `exit` running the atexit
chain so stdio flushes.

Output goes to the **console and the serial port** at once. `printf` shows up
on the screen for a person sitting at the machine, and in `serial.log` for a
headless test run.

Input works too: `read(0, ...)`, `getchar()` and `fgets(..., stdin)` all block
until a key is pressed rather than spinning. Two things to know:

- **Nothing is echoed.** The kernel does not print what you type — a program
  drawing to the framebuffer would get keystrokes painted over it. Echo what
  you want the user to see, as `apps/echoin/` does.
- **Only the foreground process is fed.** The shell grants keyboard input to
  whatever it launches and takes it back when that program exits, so a
  background process reading fd 0 simply blocks.

Graphics, through one function in `libc_glue/secureos_fb.h`:

```c
#include "secureos_fb.h"

uint32_t *px = malloc(320 * 200 * 4);   /* 0x00RRGGBB, packed, row-major */
/* ... fill px ... */
secureos_fb_present(px, 320, 200);
```

The kernel scales the buffer up by the largest integer factor that fits the
screen, centres it and puts black bars around the rest. There is nothing to
open or map first, and the buffer is not retained after the call returns. See
`apps/fbtest/` for a worked example.

Raw keyboard events, through `libc_glue/secureos_kbd.h`, for anything that
needs more than characters:

```c
#include "secureos_kbd.h"

secureos_kbd_raw(1);                  /* stdin goes quiet; events start */

int ev = secureos_getscancode();      /* -1 when nothing is buffered */
if (ev >= 0) {
    unsigned code = SECUREOS_KEY_CODE(ev);
    int down      = !SECUREOS_KEY_IS_RELEASE(ev);
    int arrow_etc =  SECUREOS_KEY_IS_EXTENDED(ev);
}

secureos_kbd_raw(0);                  /* back to characters */
```

Cooked stdin cannot report a key *release* or see the arrows at all, which is
most of what a game needs. Raw mode also stops feeding the character ring, so
the keys a game consumes do not pile up in the shell's line buffer and get
executed as a command when the game exits. Both rings are flushed entering and
leaving raw mode, and again whenever a program starts or exits — see
`apps/keytest/`.

`sleep()`, `usleep()` and `nanosleep()` work, with one caveat worth knowing
before you build timing on them: the kernel timer runs at 100 Hz and rounds a
request up to a whole tick, so the granularity is 10 ms and nothing sleeps for
less. `usleep(1)` and `usleep(9999)` both sleep ten milliseconds. Nothing can
interrupt a sleep — there are no signals — so none of them return `EINTR`, and
`nanosleep()`'s remaining-time argument always comes back zero.

Not working, and failing honestly rather than silently: `fork`, `execve`,
`wait`, signals, `unlink`, `link`, `mkdir`. These return -1 with a plausible
`errno`. There are no directories to create and no second process to start, so
a program that needs either has to do without — doom does, writing its config
and savegames into the FAT32 root.

Also worth knowing:

- **No argv from the kernel.** `create_thread` puts nothing on the user stack
  — no argc, argv, envp or auxv. `crt0.S` synthesises `argc=1`,
  `argv[0]="prog"` and an empty environment. A program needing real arguments
  has to fabricate them itself, which is what the doom backend does.
- **fds are 3..15**, sixteen slots. **Slot 3 is the executable itself** and
  cannot be closed: the kernel gives it its own fd kind and `close(3)` returns
  -1. The process's own text is demand-paged through that handle for its entire
  life. Reading and seeking it work normally.
- **The heap is 64 MiB per process**, an anonymous demand-paged reservation at
  `0x40000000`. Untouched pages cost nothing.
- **The stack is 1 MiB**, GROWDOWN and demand-paged. Deep recursion runs off
  the bottom into unmapped space and surfaces as a page fault nowhere near the
  code that caused it.
- **`errno` is approximate.** The kernel returns a flat -1 with no error code,
  so the values the glue layer sets are plausible, not accurate.

## How the pieces fit

| Path | What it is |
|---|---|
| `apps/` | one directory per program, discovered automatically by the Makefile |
| `crt0.S` | entry point: aligns the stack, zeroes `.bss`, runs constructors, calls `main`, then `exit` |
| `user.ld` | linker script: two `PT_LOAD`s, base `0x2000000`, `ENTRY(_start)` |
| `libc_glue/syscalls.c` | newlib's `_read`/`_write`/`_sbrk`/… on this kernel's `int 0x80` ABI |
| `libc_glue/secureos_syscall.h` | the raw `int 0x80` inline asm |
| `libc_glue/secureos_fb.h` | `secureos_fb_present()`, the whole graphics API |
| `libc_glue/secureos_kbd.h` | raw key events: `secureos_kbd_raw()`, `secureos_getscancode()` |
| `newlib/` | vendored `libc.a`, `libm.a`, headers — see its [README](newlib/README.md) |
| `../../hdr/syscall_abi.h` | syscall numbers and calling convention, shared with the kernel |

Two constraints the linker script exists to satisfy, if you ever write your
own: each segment's file offset must be congruent to its virtual address mod
4096 (the loader rejects anything else, because demand paging computes a
page's file offset from its address), and the base must sit above the kernel's
low identity alias at roughly `0x1c00000`. `-z max-page-size=0x1000` is
required — ld defaults to 2 MiB segment alignment on x86-64 and would break
the congruence.

Always name `ENTRY(_start)` explicitly. Without it ld picks the start of
`.text`, which is `_start` only by accident of source order and stops being
`_start` the moment you define a helper above it. The binary then runs from
the wrong symbol with a zeroed register file and dies somewhere that looks
unrelated to the cause.
