# Running doomgeneric on SecureOS

Living roadmap for the doom port. Supersedes the "Doom-generic port blockers"
section of `TODO.md`.

**Status: done. `./DOOM.EXE` plays doom.** The demo loop runs E1M1 at 320x200
with the right palette and a working HUD, the menu takes the arrow keys, and
the WAD loads off FAT32 in a quarter of a second. Everything below is now a
record of how it was built rather than a plan.

---

## Where things stand

```
Phase 0  toolchain & vendoring     newlib DONE   doomgeneric DONE   WAD DONE
Phase 1  kernel blockers           9 of 9 DONE
Phase 2  userland build path       DONE
Phase 3  newlib retargeting        DONE
Phase 4  doomgeneric backend       DONE
Phase 5  milestones                0 through 6 DONE
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
| `CAT.EXE` | **FAT32 through newlib stdio**: the 4 MB WAD read, seeked and checksummed, and `close(3)` refused |
| `DOOM.EXE` | **everything** |

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

`CAT.EXE`'s:

```
[CAT] step 1: opening /DOOM1.WAD
[CAT] step 2: size 4196020 bytes
[CAT] step 3: header ok, 1264 lumps, directory at 4175796
[CAT] step 4: first lump PLAYPAL, 10752 bytes at 12
[CAT] step 5: 4196020 bytes read, checksums match, 230 ms, 17813 KB/s
[CAT] step 6: close(3) on the executable refused
[CAT] done, exiting 0
```

The checksums are computed on the host from the same file, so a single wrong
byte anywhere in the chain walk fails the test here rather than turning into
doom misbehaving for no visible reason twenty minutes later.

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

### Hardening, done

- **`_fstat`** reports `S_IFCHR` for fds 0/1/2, and `S_IFREG` with a size
  recovered by `lseek(END)` for files. Doom stats the WAD and stdio sizes its
  reads from `st_blksize`, so both spellings had to be honest.
- **fd 3 cannot be closed.** `PROC_FD_EXEC_IMAGE` (`hdr/p.h`) is a separate fd
  kind for the executable the loader pins at `PROC_FD_BASE`. It reads, writes
  and seeks exactly like `PROC_FD_FILE` — `fd_slot_is_file()` in
  `fs_syscalls.c` is what every operation except close asks — and `sys_close`
  refuses it. Teardown in `release_threadless_proc()` closes it directly,
  bypassing the syscall. Regression test: `CAT.EXE` step 6.

  The risk was never an overwrite: `fd_slot_alloc()` hands out the first free
  slot and this one is in use from the moment the process exists. It was that
  a `close(3)` from ring3 removes the backing store the calling code is being
  demand-paged from, and the next text fault reads through a closed handle.

### Outstanding hardening

- No user-pointer validation in the syscall layer. Worth a
  `< USER_SPACE_LIMIT_` check on `read`/`write`/`fb_present` buffers.
  `sys_fb_present` already does its own; `read`/`write` do not.

---

## Phase 0 — vendoring

**doomgeneric** lives in `src/userland/apps/doom/`, upstream `dcb7a8d`, flat:
the app discovery in the Makefile compiles every `.c` directly inside an app
directory and does not descend, which is exactly doomgeneric's own layout. The
file list is the one `Makefile.soso` uses — the other hobby-OS port — minus its
backend, so the platform backends (`_sdl.c`, `_xlib.c`, `_win.c`, `_soso.c`, …)
and the SDL sound and music modules are simply not vendored.

Two files needed a decision rather than a copy:

- **`i_joystick.c`** is SDL top to bottom and is replaced by a local file of
  the same name holding four empty functions. Stubbing it there rather than
  deleting the `I_InitJoystick()` calls in `d_main.c` and `i_system.c` is what
  keeps the vendored sources diffable against upstream.
- **`net_sdl.h`** and **`i_joystick.h`** are kept. Both are referenced by
  files that are compiled, neither includes an SDL header, and the one symbol
  `net_sdl.h` declares is never linked against.

Resolution and warnings come from `doom_CFLAGS` in `src/boot/Makefile`:
`-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -w`. 320x200 is
doom's native mode and the kernel scales it on presentation, so rendering
larger would cost work and gain nothing. `-w` silences roughly two thousand
warnings in third-party code nobody here is going to fix; every other app
still builds `-Wall -Wextra`.

**`DOOM1.WAD`** is not committed. `make doom-wad` fetches the shareware v1.9
WAD into a gitignored `assets/`, checks it against a sha256, and only then
renames it into place; the FAT32 image depends on it and copies it to
`/DOOM1.WAD`. Downloading to a `.part` file first is what keeps an HTML error
page saved under the WAD's name from being cached as a finished artifact that
make then considers up to date forever. The checksum is not decoration either:
several different files are called `doom1.wad`, and doom misbehaves on the
wrong one in ways that look like a bug in the port.

---

## Phase 4 — the backend

`src/userland/apps/doom/doomgeneric_secureos.c`:

| Function | Implementation |
|---|---|
| `DG_Init` | `SYS_kbd_raw(1)` |
| `DG_DrawFrame` | `SYS_fb_present(DG_ScreenBuffer, 320, 200)`, then drain the key ring |
| `DG_GetTicksMs` | `SYS_gettime` |
| `DG_SleepMs` | `SYS_sleep` |
| `DG_SetWindowTitle` | no-op |
| `DG_GetKey` | pop one event from the queue `DG_DrawFrame` fills |

The kernel ring is drained in `DG_DrawFrame` rather than in `DG_GetKey`
because `DG_DrawFrame` is the one entry point doom is guaranteed to call every
frame, while `DG_GetKey` is called until it returns zero — draining there
would mean the ring is only ever emptied as fast as doom asks, and a key held
down across a slow frame would arrive late.

Translation is a switch on the set-1 scancode, with the `0xE0` prefix taken
from `SECUREOS_KEY_EXTENDED` selecting a second table: extended `0x48` is the
up arrow, plain `0x48` is keypad 8, and both map to `KEY_UPARROW` because doom
wants either to work. `SECUREOS_KEY_RELEASED` becomes doomgeneric's `pressed`
flag, which is what makes movement keys repeat while held rather than
stuttering.

The kernel supplies no argv, so the backend provides `main()`:

```c
char *argv[] = { "doom", "-iwad", "/DOOM1.WAD", NULL };
doomgeneric_Create(3, argv);
```

`-iwad` rather than letting `d_iwad.c` search: its search walks directories
this filesystem does not have.

### What newlib had to grow

Linking doom turned up four missing symbols, all added to
`src/userland/libc_glue/syscalls.c`:

- **`mkdir`** — `M_MakeDirectory()` wants somewhere for savegames and a config
  file. There is no mkdir syscall and the FAT32 layer creates files but not
  directories, so it fails with `ENOSYS`. Doom copes by writing to the current
  directory, which is the FAT32 root and is where everything lives anyway.
- **`fork`, `wait`, `execve`** — unprefixed aliases of the already-failing
  underscore versions. Nothing in doom calls them; newlib's `system()` and its
  reentrant exec wrappers reference them, and linking anything that pulls in
  `system()` drags all three in.

---

## Phase 4a — making the WAD readable in finite time

Doom opens a 4 MB file and then seeks all over it, and the FAT32 read path was
quadratic in the file size. Three changes, all in the kernel, took a full read
of the WAD from "unusable" to 240 ms:

**A one-sector FAT cache in the mount** (`fat32_mount_t::fat_cache`). Walking a
cluster chain reads one FAT entry per cluster and a 512-byte FAT sector holds
128 of them, so an uncached walk re-read the same sector 128 times in a row —
and a read is a polled PIO transfer, not a memory access. One entry is enough
because chain walks are sequential by nature. It is filled through a const
pointer, deliberately: every FAT reader in the tree holds the mount as const,
and the cache is not part of the filesystem's logical state.

**A per-file chain hint** (`fat32_file_t::read_hint`). Without it every read
walks from the first cluster, so reading a 4 MB file in 4 KB pieces walks the
chain from the beginning a thousand times. The hint makes the common case — the
next read continues where the last stopped — a walk of zero steps. It is only
ever written from a position reached by following the chain, so the only thing
that can invalidate it is truncation, which resets it.

**Contiguous multi-sector reads.** `fat32_read_file_at()` now finds the longest
run of physically consecutive clusters and reads it in one `block_device_read`,
and `ide_pio_read()` issues one READ SECTORS command per run instead of one per
sector. A freshly formatted image lays files out in a single run, so the WAD is
8192 consecutive clusters. The bounce buffer is 8 sectors because that is
kmalloc's ceiling — `kmalloc` refuses anything over 4096 (`heap.c`), and asking
for more silently falls back to the one-sector path.

Reads go through that bounce buffer rather than straight into the caller's
buffer on purpose: `sys_read`'s destination is a userland pointer whose pages
may be demand-paged and not yet present, and the IDE driver refuses a
destination that is not mapped — it cannot resolve a fault from inside its
transfer loop.

---

## Phase 5 — milestones

| # | Binary | Proves | Status |
|---|---|---|---|
| 0 | `RING3M.EXE` | mmap ABI from ring3, `.bss` genuinely zeroed | **done** |
| 1 | `HELLO.EXE` | newlib, crt0, `_write`, SSE, ELF > 4 KB | **done** |
| 2 | `RING3T.EXE` | `SYS_brk`, demand-paged anon | **done** (assembly; a `MALLOC.EXE` doing 16 MB through malloc would be a stronger check) |
| 3 | `CAT.EXE` | FAT32 read of a multi-MB file through newlib stdio | **done** (4 MB in 240 ms, checksums verified) |
| 4 | `FBTEST.EXE` | `SYS_fb_present`, scaling, blit throughput | **done** (100 fps at 320x200 2x) |
| 5 | `KEYTEST.EXE` | press *and* release, arrows, `0xE0`, shell not stealing input | **done** |
| 6 | `DOOM.EXE` | everything | **done** |

**Definition of done**, met: `./DOOM.EXE` from the shell reaches the title
screen, the demo loop plays E1M1 with the correct palette and HUD, and the
arrow keys and Ctrl work.

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

- **WAD loading was the predicted bottleneck and is not one any more** — see
  Phase 4a. A full 4 MB read is 240 ms. If doom runs slowly now, look at its
  own rendering under `-O0`, not at `fb_blit_scaled` (100 fps at 2x, milestone
  4) and not at the read path.
- **Protection faults panic the whole kernel** (`pf_panic`). Doom *will* fault
  during bring-up and every fault costs a reboot. Out of scope, but if the
  reboot cycle becomes the bottleneck, promoting protection faults to a
  process kill is the highest-value scope addition available.
- **`krealloc` is declared but never defined** (`hdr/heap.h`) — a link error
  waiting for whatever kernel code reaches for it first.
- **A header change used to rebuild nothing**, and that is not a slow build,
  it is a wrong one. Change a field in a struct that lives in a header — `proc`,
  `thread`, `fat32_file_t` — and every object compiled before the edit keeps
  the old layout while the ones after use the new one. The kernel links happily
  and reads fields at different offsets depending on which file is asking; the
  symptom here was `sbrk` returning -1 in every ring3 program because `sys_brk`
  and the loader disagreed about where `proc::brk_base` lives. The Makefile now
  generates `.d` files with `-MMD -MP` and includes them.

  That fix has a trap of its own, already sprung: `PROJECT_ROOT` was defined
  with `=` from `$(lastword $(MAKEFILE_LIST))`, which is re-evaluated at use
  time — and once the generated `.d` files are included, the last file make
  read is one of them, so the project root resolved to `build/` and took
  `-I$(INCLUDE_DIR)` with it. It is `:=` now.

- **No `ENTRY()` in a linker script is a trap.** ld falls back to the start of
  `.text`, which is `_start` only by accident of source order and stops being
  `_start` the moment a helper is defined above it. The binary then runs from
  the wrong symbol with a zeroed register file and dies somewhere unrelated.
  `user.ld` and `elf_ring3_sse_test.ld` both name it explicitly now.
