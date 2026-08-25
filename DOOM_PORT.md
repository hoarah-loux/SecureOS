# Running doomgeneric on SecureOS

Living roadmap for the doom port. Supersedes the "Doom-generic port blockers"
section of `TODO.md`.

**Status: milestone 5 reached, all nine kernel blockers done.** A C program
runs in ring3 against a real libc, puts pixels on the screen at 320x200 faster
than doom needs, and reads the keyboard as raw press/release events including
the arrows. What is left is doomgeneric itself.

---

## Where things stand

```
Phase 0  toolchain & vendoring     newlib DONE   doomgeneric TODO   WAD TODO
Phase 1  kernel blockers           9 of 9 DONE
Phase 2  userland build path       DONE
Phase 3  newlib retargeting        DONE (fstat/fd-3 hardening outstanding)
Phase 4  doomgeneric backend       TODO
Phase 5  milestones                0, 1, 2, 4 and 5 covered; 3 and 6 TODO
```

### What runs today

Every one of these is on the FAT32 image and launched from the shell with
`./NAME`. All pass.

| Binary | What it proves |
|---|---|
| `RING3.EXE` + `RING3P.EXE` | timer preemption interleaves two ring3 processes |
| `RING3S.EXE` | user stack: push/pop, frames, page crossing |
| `RING3M.EXE` | demand paging: multi-page text, `.data`, `.bss` tail, anon region |
| `RING3B.EXE` | 25 KB binary — the old 4 KB loader ceiling is gone |
| `RING3F.EXE` + `RING3G.EXE` | SSE executes, and xmm0-7 survive preemption |
| `RING3T.EXE` | `SYS_brk` grow/zero/bounds, `SYS_gettime`, `SYS_sleep` |
| `HELLO.EXE` | **newlib**: printf, varargs `%f`, malloc, `.bss`, strdup, exit |
| `GREET.EXE` | the worked example in `src/userland/README.md` still builds |
| `FBTEST.EXE` | **`SYS_fb_present`**: scaling, centring, letterbox, rejection of bad buffers, throughput |
| `ECHOIN.EXE` | **blocking `fd 0`**: `read()` and `fgets()` on the keyboard, shell does not steal keys |
| `KEYTEST.EXE` | **raw scancodes**: press *and* release, arrows, `0xE0`, raw mode keeping the shell clean |

`HELLO.EXE`'s actual output:

```
[HELLO] step 1: printf works
[HELLO] step 2: argc=1 argv[0]=prog
[HELLO] step 3: -42 str x 0xdeadbeef 3.142
[HELLO] step 4: malloc+memset 65536 bytes, 0 mismatches
[HELLO] step 5: .bss 8192 bytes, 0 nonzero
[HELLO] step 6: strdup=secureos len=8
[HELLO] done, exiting 0
```

Step 3 prints `3.142` only because SSE is enabled and saved across context
switches; step 5 reads zero only because of the `p_filesz` clamp; step 4 is
malloc on a real `SYS_brk`. Each step is load-bearing.

`FBTEST.EXE`'s, on an 800x600 screen:

```
[FBTEST] step 1: buffer 320x200 at 0x40000010
[FBTEST] step 2: first frame presented
[FBTEST] step 3: second frame presented
[FBTEST] step 4: kernel address rejected
[FBTEST] step 5: unmapped extent rejected
[FBTEST] step 6: zero width rejected
[FBTEST] step 7: 60 frames in 600 ms = 100 fps
```

---

## Phase 1 — kernel blockers

### Done

**§1.1 Loader 4 KB ceiling.** `load_file()`/`parse_mem_elf()` are gone. The
loader reads only the 64-byte ELF header into kernel memory and seeks to each
program header individually (`elf_open_image`, `elf_read_at`,
`src/kernel/proc/loader/loader.c`). Loader memory use is constant regardless
of executable size. Regression test: `RING3B.EXE`.

**§1.2 `.bss` got file garbage instead of zeros.** `populate_fault_frame()`
read a full 4096 bytes from the backing file with no clamp at `p_filesz`, so
`.bss` came back as whatever followed the segment on disk. `vm_region` gained
`file_valid_len`; the fault handler reads `min(4096, file_valid_len -
page_delta)` and zero-fills the rest. This also removes any need to split a
`PT_LOAD` into file-backed plus anonymous regions — one region with an honest
valid length handles the partial page a split cannot. Regression test:
`RING3M.EXE` step 3.

**§1.3 SSE.** `cpu_enable_sse()` (`src/arch/x86_64/cpu_sse.c`) clears `CR0.EM`,
sets `CR0.MP`, `CR4.OSFXSR`, `CR4.OSXMMEXCPT` and runs `FNINIT`, called early
in `kmain` — before `idt_init()`, because `restore_context` now uses `FXRSTOR`
and would `#UD` on the first context switch otherwise. Each thread carries a
512-byte FXSAVE area; `save_trap_frame()` does the `fxsave`, `restore_context`
the `fxrstor`. Eager, not lazy.

Regression test: `RING3F.EXE` and `RING3G.EXE`, **run together**. One SSE
process would pass even with no vector state saved at all, since the kernel is
`-mno-sse` and the shell is kernel code. Two with different seeds clobber each
other unless it genuinely works — verified by removing the `fxrstor` and
watching copy [0] fail at step 4.

**§1.4 `sys_mmap` was not callable from ring3.** `mmap_req` carried a raw
kernel `thread *t` that `sys_mmap` dereferenced unvalidated — an arbitrary
kernel-pointer read from userspace, and a field no user program could fill in
honestly. The struct is gone; `mmap()` takes its six caller-chosen arguments
directly (matching the six syscall registers) plus a kernel-supplied `thread *`:

```c
void *mmap(thread *t, void *addr, size_t len, uint64_t perms,
           uint64_t map_flags, uint64_t fd, uint64_t file_offset);
```

The target stays a `thread` rather than becoming a `vm_space` because
`reserve_region()` threads each region onto `t->t_region_list`. The loader
passes the thread it just created; `sys_mmap` resolves it from the scheduler,
which is safe there because a syscall arriving from ring3 means something is
running by definition.

**§1.5 `SYS_brk`.** One 64 MiB anonymous demand-paged reservation per process
at `USER_BRK_BASE` (`0x40000000`), made by `elf_load_process()`; `brk()` moves
a cursor inside it. No mapping work, no region churn, nothing to release —
which is why malloc works with `SYS_munmap` still a stub.

**§1.6 `SYS_gettime` / `SYS_sleep`.** `gettime` is
`timer_ticks_get() * TIMER_MS_PER_TICK` (10 ms resolution, PIT at 100 Hz).
`sleep` blocks on the scheduler's sleep queue, drained once per tick by
`scheduler_wake_expired_sleepers()`.

Watch out: `syscall_handler` rewinds RIP past the `int 0x80` before blocking,
so a woken thread **re-executes the whole syscall**. Any new blocking syscall
must be re-entrant. `SYS_sleep` carries `thread::wake_tick` to tell a
continuation from a fresh request; without it a sleeper resets its own clock
every wake and never returns.

**Userland console I/O.** Not one of the nine blockers, but doom needs it and
so does anything interactive. `sys_write` on fds 1 and 2 used to go only to
`serial_write_char`, so a program's output existed only on the wire — the
console object the shell draws with was a `static` in kmain's translation unit
that the syscall layer could not see. `console_set_active()` /
`console_get_active()` fix that the same way `fb_set_runtime()` did, and output
now goes to **both** the console and serial. Duplicated rather than moved:
serial is the only output a headless run can read, and every test here works by
grepping `serial.log`.

`sys_read` on fd 0 used to drain whatever the ring buffer held and return
immediately — zero if it held nothing — so a program waiting for input burned a
whole CPU and newlib's stdio never made progress. It now returns
`SYS_WOULD_BLOCK` on an empty read and `syscall_handler` parks the thread on the
keyboard queue. Blocking strictly *before* any byte is consumed is what makes it
re-entrant under the RIP rewind, so unlike `SYS_sleep` it needs no continuation
state.

The kernel deliberately does **not** echo. A program drawing to the framebuffer
must not get keystrokes painted over it, and echo belongs with a line
discipline, which is TTY-shaped work this is not.

**§1.7 `SYS_fb_present`.** Userland hands the kernel a buffer of 32-bit
pixels and the kernel draws it. The copy is not avoidable yet: the framebuffer
is mapped into the physmap without `PTE_US` and `sys_mmap` has no device-backed
path, so there is no way to give a process the real pixels.

The framebuffer tag moved out of `kernel.c` — where it was a `static` the
syscall layer could not see — into `src/kernel/core/framebuffer.c` behind
`fb_set_runtime()` / `fb_get_runtime()`. `console_init()` takes a `const`
pointer now, since it never wrote to the tag anyway.

`fb_blit_scaled(fb, src, w, h)` picks the largest integer scale that fits,
`s = min(fb_w/w, fb_h/h)`, centres the result and leaves black bars. Resolution
is not pinned — `grub.cfg` uses `gfxmode=auto`, and QEMU's OVMF path comes up
at 800x600, which is 2x rather than the 3x a 1024x768 screen would give.

Three things in the blit are there for speed, and all three matter because the
kernel builds at `-O0`, where nothing written is optimised away:

- `fb_putpixel()` is not used. It re-validates coordinates and re-runs a
  bits-per-pixel switch per pixel.
- Copies move `uint32_t`, not bytes. `hdr/string.h`'s `memcpy` is a byte loop.
- Each group of `s` output rows is identical, so only the first is built pixel
  by pixel and the rest are copied from it. That is what keeps the per-pixel
  cost proportional to `w*h*s` instead of `w*h*s*s`.

A generic `fb_putpixel` path is kept for 16 and 24 bpp. The letterbox is
cleared only when the destination geometry changes, not every frame.

`sys_fb_present(buf, w, h)` bounds `w` and `h` first (so `w*h*4` cannot
overflow), then checks the whole extent against the calling process's
`vm_space::user_limit`, then confirms both the first and last byte land in one
of its `vm_region`s. The last check is not paranoia: an address below
`user_limit` but in no region faults in kernel mode, and a `#PF` the handler
cannot resolve is `pf_panic`, which kills the machine rather than the process.
A buffer straddling two regions with a hole between them is not caught — that
is a deliberate trade against a page-by-page walk on the hot path.

Measured at 100 fps for 320x200 at 2x on the QEMU target. At 3x the same work
is roughly 2.25x larger, which still clears doom's 35 Hz.

**§1.8 Keyboard raw scancodes.** A second ring of `uint16_t` fed by the same
interrupt as the cooked one, so the shell keeps reading characters while a game
reads events. Encoding: bits 0-7 the set-1 scancode with the release bit
stripped, bit 8 `KBD_SC_EXTENDED` (introduced by an `0xE0` prefix), bit 15
`KBD_SC_RELEASED`. `'A'` pressed is `0x001E`, released `0x801E`; up-arrow
pressed is `0x0148`.

`SYS_getscancode` (non-blocking poll) and `SYS_kbd_raw(on)` were appended
before `SYS_MAX`, so nothing renumbered. Both are gated on input ownership.

Two details that are not obvious and cost real debugging if got wrong:

- **Fake shifts.** PrintScreen and the arrow keys emit `0xE0 0x2A` / `0xE0 0xAA`
  for the benefit of DOS-era software. Treating those as a real shift press
  capitalises the next character for no reason the user can see, so the shift
  handler ignores anything carrying the extended flag.
- **Pause/Break** arrives as `0xE1` plus a six-byte sequence with no release at
  all. It is not decoded; the prefix is dropped and its payload bytes fall
  through as ordinary keys. Nothing here binds it.

**Arbitration** is what makes all of this usable, and it is not part of the
scancode work — it is `§1.8a` below.

**§1.8a Input ownership.** One keyboard, one buffer, and more than one thread
that wants it: the shell sits in `SYS_getkey_wait` for its whole life and any
program it launches wants the same keystrokes. Whichever thread the scheduler
woke first took the character, which is not a race that can be tuned away.

Exactly one process owns input. Everyone else's read blocks. The shell is the
default owner, grants input to whatever it launches, and gets it back from
`scheduler_exit_current()` — on the exit path rather than in the shell, because
that is the choke point every exit passes through, so a program that dies badly
cannot take the keyboard with it.

The keyboard IRQ now calls `scheduler_wake_all()` rather than
`scheduler_wake_one()`. wake_one pops the head, and the head may be a thread
that does not own input: it wakes, sees it still does not, and blocks again
while the keystroke that woke it sits unread and the process entitled to it
stays asleep. One key press, nobody served.

Both rings are flushed and raw mode is cleared whenever ownership moves, so a
game's movement keys never arrive at a shell prompt as a line of text.

**§1.9 User stack.** 64 KB → 1 MB (`USER_STACK_SIZE`). GROWDOWN and
demand-paged, so this is a recursion-depth ceiling rather than an allocation.

### Remaining

---

## Phase 3 — newlib retargeting

Done and working (`src/userland/libc_glue/syscalls.c`). Three things were not
obvious and are worth not rediscovering:

- **Include order is load-bearing.** gcc searches `-I` before `-isystem`, so
  putting the kernel's `hdr/` in with `-I` made `<string.h>` resolve to the
  kernel's. It goes in with `-idirafter` instead. Userland needs `hdr/` for
  exactly one file, `syscall_abi.h`.
- **Both spellings of every syscall.** With
  `--disable-newlib-supplied-syscalls`, newlib's reentrant layer calls
  `read()`/`write()`/`sbrk()` while every retargeting convention supplies
  `_read()`/`_write()`/`_sbrk()`. The glue aliases them.
- **`_init()`** is called by `__libc_init_array()` and normally comes from
  `crti.o`, which does not exist without a full cross toolchain. It is an
  empty function here; `user.ld` collects constructors into `.init_array`,
  which `__libc_init_array` walks directly.

### Outstanding hardening

- `_fstat` should report `S_IFCHR` for fds 0/1/2 and `S_IFREG` plus a size
  from `lseek(END)` for files. Doom stats the WAD.
- **fd 3 must never be closed.** The loader pins the executable there and
  every not-yet-touched text page is still demand-paged through it. A stray
  `close(3)` from ring3 tears out the backing store the process is running
  from, and the next fault reads through a closed handle. Add a fd kind
  (`PROC_FD_EXEC_IMAGE`) that `sys_close` refuses and teardown bypasses.
  No overwrite risk exists — `fd_slot_alloc()` scans for the first free slot
  and skips the pinned one — only a close risk.
- No user-pointer validation in the syscall layer. Worth a
  `< USER_SPACE_LIMIT_` check on `read`/`write`/`fb_present` buffers.

---

## Phase 0 — what is left

- **doomgeneric**: vendor into `src/userland/doom/` at `dcb7a8d`. Delete the
  platform backends (`doomgeneric_sdl.c`, `_xlib.c`, `_win.c`, …); keep
  `doomgeneric.c`, `doomgeneric.h` and the DOOM sources. Set
  `DOOMGENERIC_RESX 320` / `RESY 200`.
- **`DOOM1.WAD`**: shareware, ~4 MB, into a gitignored `assets/`. Do **not**
  commit it. Add a Makefile rule that fetches it and errors clearly if absent.
  Lands on the image as `/DOOM1.WAD` (FAT 8.3).

---

## Phase 4 — the backend

`src/userland/doom/doomgeneric_secureos.c`:

| Function | Implementation |
|---|---|
| `DG_Init` | `SYS_kbd_raw(1)` |
| `DG_DrawFrame` | `SYS_fb_present(DG_ScreenBuffer, 320, 200)` |
| `DG_GetTicksMs` | `SYS_gettime` |
| `DG_SleepMs` | `SYS_sleep` |
| `DG_SetWindowTitle` | no-op |
| `DG_GetKey` | drain `SYS_getscancode`, translate to doomkey, `*pressed` from the release bit |

Doomkeys: `KEY_RIGHTARROW 0xae`, `KEY_LEFTARROW 0xac`, `KEY_UPARROW 0xad`,
`KEY_DOWNARROW 0xaf`, `KEY_FIRE`/RCtrl `0x9d`, `KEY_USE` space,
`KEY_ESCAPE 27`, `KEY_ENTER 13`, `KEY_TAB 9`, plus alphanumerics.

The kernel supplies no argv, so the backend provides `main()`:

```c
char *argv[] = { "doom", "-iwad", "/DOOM1.WAD", NULL };
doomgeneric_Create(3, argv);
```

---

## Phase 5 — remaining milestones

| # | Binary | Proves | Status |
|---|---|---|---|
| 0 | `RING3M.EXE` | mmap ABI from ring3, `.bss` genuinely zeroed | **done** |
| 1 | `HELLO.EXE` | newlib, crt0, `_write`, SSE, ELF > 4 KB | **done** |
| 2 | `RING3T.EXE` | `SYS_brk`, demand-paged anon | **done** (assembly; a `MALLOC.EXE` doing 16 MB through malloc would be a stronger check) |
| 3 | `CAT.EXE` | FAT32 read of a multi-MB file through newlib stdio | todo |
| 4 | `FBTEST.EXE` | `SYS_fb_present`, scaling, blit throughput | **done** (100 fps at 320x200 2x) |
| 5 | `KEYTEST.EXE` | press *and* release, arrows, `0xE0`, shell not stealing input | **done** |
| 6 | `DOOM.EXE` | everything | todo |

**Definition of done**: `./DOOM.EXE` from the shell reaches the title screen,
the demo loop plays, and arrow keys + Ctrl move and fire in-game.

---

## Testing headlessly

The shell is fed by the keyboard IRQ, so a `-display none` QEMU run has no way
to launch anything. `SHELL_AUTORUN` runs one command — or several, separated
by `;` — before the interactive loop starts:

```sh
cd src/boot
make CFLAGS_EXTRA='-DSHELL_AUTORUN="\"./HELLO.EXE\""'
make "$PWD/build/secureos.fat32.img"      # absolute path required, see below

qemu-system-x86_64 -machine pc \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
  -boot order=d -cdrom ../../secureos-uefi.iso \
  -drive file=build/secureos.fat32.img,format=raw,if=ide,index=0,media=disk \
  -serial file:serial.log -display none -m 2048 \
  -d int,cpu_reset -no-reboot -D qemu.log
```

Three things that will otherwise cost you an hour each:

- **make does not track `CFLAGS`.** Changing `SHELL_AUTORUN` alone rebuilds
  nothing. `touch src/kernel/core/kernel.c` first.
- **The FAT32 image must be named by its absolute path.** `$(FAT32_IMAGE)`
  expands under an absolute `BUILD_DIR`, so `make build/secureos.fat32.img`
  does not match the rule, and the image is not a prerequisite of `all`.
- **`build/OVMF_VARS_4M.fd` must exist and be writable.** Copy it from
  `/usr/share/OVMF/OVMF_VARS_4M.fd`.

Drop `-display none` to watch. `-d int,cpu_reset -no-reboot` is what makes
triple faults findable — the last `check_exception` chain plus the register
dump gives RIP, RSP, CR2 and CR3. Resolve a faulting RIP with
`nm -n src/boot/build/secureos.elf` and subtract `0xFFFFFFFF80000000`.

### Driving the keyboard

`SHELL_AUTORUN` starts a program, but a program that waits for input needs
input. QEMU's monitor will type for you:

```sh
printf 'sendkey a\nsendkey up\nsendkey ctrl\nsendkey esc\n' \
  | socat - unix-connect:/tmp/mon.sock
```

Key names are QEMU's own (`up`, `down`, `left`, `right`, `ctrl`, `esc`, `ret`,
`spc`, letters and digits as themselves). Leave 100 ms or so between keys.
`KEYTEST.EXE` and `ECHOIN.EXE` are both written to be driven this way.

### Checking what is on the screen

Serial says nothing about pixels. QEMU's monitor will dump the framebuffer to a
PPM, which is a format simple enough to check from a few lines of Python — so a
graphics test can assert exact colours at exact coordinates rather than being
eyeballed. Add `-monitor unix:/tmp/mon.sock,server,nowait` to the run above,
wait for the program to print that it is holding a frame, then:

```sh
printf 'screendump /tmp/shot.ppm\nquit\n' | socat - unix-connect:/tmp/mon.sock
```

`FBTEST.EXE` is built for this: flat colours with hard edges at known
coordinates, and a deliberate pause at the end, because the shell writes to the
console — the same framebuffer — the moment the process exits.

Program output goes to **serial**, not the console: `sys_write` sends fd 1 to
`serial_write_char`. Load failures are printed to serial too, which matters —
the framebuffer console is invisible headlessly, and a failed load otherwise
looks exactly like a program that started and printed nothing.

---

## Known risks

- **`-O0` kernel plus polled single-sector PIO IDE** means WAD loading will be
  slow. The blit itself is no longer the worry — milestone 4 measured 100 fps
  at 2x — so if doom runs slowly, look at the WAD read path and at doom's own
  rendering, not at `fb_blit_scaled`.
- **Protection faults panic the whole kernel** (`pf_panic`). Doom *will* fault
  during bring-up and every fault costs a reboot. Out of scope, but if the
  reboot cycle becomes the bottleneck, promoting protection faults to a
  process kill is the highest-value scope addition available.
- **`krealloc` is declared but never defined** (`hdr/heap.h`) — a link error
  waiting for whatever kernel code reaches for it first.
- **No `ENTRY()` in a linker script is a trap.** ld falls back to the start of
  `.text`, which is `_start` only by accident of source order and stops being
  `_start` the moment a helper is defined above it. The binary then runs from
  the wrong symbol with a zeroed register file and dies somewhere unrelated.
  `user.ld` and `elf_ring3_sse_test.ld` both name it explicitly now.
