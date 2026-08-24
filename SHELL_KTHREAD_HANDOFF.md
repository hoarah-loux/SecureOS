# Handoff: kernel shell as a kthread / idle thread

Context for a fresh session. Goal: turn the shell into a kernel thread and register it
as the scheduler's idle thread, so control always returns to it when nothing else is
runnable.

---

## Where things stand

Ring3 preemption works end to end. Two user processes loaded from ELF files interleave
under timer preemption, both exit via `SYS_exit`, and the ready queue drains. Verified in
QEMU: 16 rounds of fully interleaved `[RING3-A]` / `[RING3-B]` output, then both threads
exit cleanly.

The path that now works: `elf_load_process` → `mmap` (file-backed, demand-paged) →
`#PF` → `populate_fault_frame` reads from the FAT32 file → user code runs at CPL3 →
`int 0x80` → `syscall_handler` → `sys_exit` → `int $0x82` → `exit_handler` →
`scheduler_exit_current` → `restore_context`.

---

## Bugs found and fixed this session

**1. ELF segments were zero-filled instead of loaded.** `elf_map_segments` passed
`file_descriptor = 0` to `mmap`, which means an anonymous region. `populate_fault_frame`
only reads file bytes when `object->type == FILE && backing_ctx != NULL`, so every
segment page came back as 4096 zero bytes. The process then executed `00 00` =
`add BYTE PTR [rax], al` with `rax = 0`, faulting on VA `0x0`. Fixed by passing a real
fd and a segment file offset.

**2. `fs_mmap_pin_fd` / `fs_mmap_file_key_from_fd` resolved the wrong process.** Both used
`get_p()` (= `scheduler_current()->owning_proc`), but the loader runs before anything is
scheduled and needs the *new* proc's fd table. Both now take a `proc *`, and `mmap` passes
`t->owning_proc`.

**3. Segment file offset had to be page-aligned against `p_vaddr`.** `seg_start` is
`p_vaddr & ~0xFFF`, and `populate_fault_frame` indexes from `region->va_start`, so the
offset belonging at `va_start` is `p_offset - (p_vaddr & 0xFFF)`. `loader.c` now rejects a
phdr where `p_offset !≡ p_vaddr (mod 4096)` rather than letting the subtraction underflow.

**4. `SYS_exit` was a stub.** Now a real `sys_exit` in `syscall.c` that records
`exit_status` and does `int $EXIT_VECTOR` (0x82), reusing the existing
`exit_isr` → `exit_handler` → `scheduler_exit_current` teardown instead of duplicating it.

**5. `restore_context` pushed a 3-word IRET frame for kernel threads.** This was the
triple fault at the handoff to idle. In long mode `iretq` **always** pops five qwords
(RIP, CS, RFLAGS, RSP, SS) even with no privilege change — unlike 32-bit protected mode.
The kernel path pushed only three, so `iretq` popped RSP and SS from uninitialised stack
above the frame, giving `RSP = 0`. The idle thread reached `idle_thread_entry` and
survived until the next timer IRQ, which then couldn't push its frame at `0 - 8` →
`#PF` → `#DF` → triple fault → reboot. `.build_kernel_iret_frame` now pushes five words
using `CTX_SS` and `CTX_KERNEL_RSP`.

Only the idle thread hit this, because it is the only thread with `cs = 0x08`. A and B use
`cs = 0x1B` and always took `.build_user_iret_frame`, which was already correct.

**6. `save_trap_frame`'s CPL0 branch mishandled SS and RSP.** It set `ss = 0` and computed
`kernel_rsp = (uintptr_t)&frame->rflags + sizeof(uint64_t)`, both on the assumption that a
same-privilege interrupt doesn't push SS:RSP. It does — that is the counterpart of the
`iretq` behaviour above. Now `ss = frame->ss` and `kernel_rsp = frame->rsp`.

The old arithmetic yielded the *address of* the rsp slot (`frame+144`) rather than its
*value* (the true pre-interrupt RSP), 16 bytes short, which would have walked the kernel
stack down 16 bytes per CPL0 preemption. It is also unfixable by arithmetic in general:
in 64-bit mode the CPU aligns RSP to 16 bytes before pushing the frame, so the frame base
is not reliably `old_rsp - 40`. The pushed value is always correct.

---

## Key facts about this codebase

- **Kernel links at 1M** (`src/boot/grub/linker.ld`) and runs from the higher-half alias
  `0xFFFFFFFF80000000`. Taking a function's address *inside a function* compiles to a
  RIP-relative `lea` and therefore already resolves to the higher half — **no manual
  `+ KERNEL_IMAGE_BASE` needed**. `idt.c` and `syscall.c` add it by hand only because their
  tables are static initialisers, which must be link-time constants.
- **Kernel-half page tables are shared.** `create_vm_space` copies PML4 slots
  `[ker_index..511]` from `kroot` by value, so all spaces point at the same PDPTs. A kernel
  stack mapped through one proc's PML4 is visible from every other.
- **Kernel stacks** are `KERNEL_ASSET` regions from a global list, allocated upward from
  `physmap_end`, 16 KiB each, eagerly mapped (they can never be demand-faulted — the `#PF`
  handler runs on the current stack at CPL0). Observed: idle at `0xFFFF800100000000`,
  next thread at `+0x4000`, and so on.
- **Quantum** is `SCHEDULER_DEFAULT_QUANTUM_TICKS = 5` at 100 Hz PIT = 50 ms.
- **Timer preemption is fully wired** in `timer_handler` (`save_trap_frame` →
  `scheduler_add_ready` → `scheduler_pick_next` → `restore_context`). Nothing manual is
  needed to make threads preempt — two runnable threads is the whole setup.
- **`scheduler_add_ready` already no-ops** for `t == cpu->idle_thread`, so the idle thread
  cannot be enqueued by the timer path.

---

## The task

Make the shell a kernel thread and register it as the idle thread.

### Why it is needed

`shell_exec_binary` in `src/kernel/core/shell.c` currently ends with:

```c
thread* first = scheduler_pick_next(0);
restore_context(first->context);
```

`restore_context` never returns, so `kmain`'s stack is abandoned and there is no saved
context to come back to. The shell is not a thread, so nothing can schedule it.

### Shape of the change

1. Hoist `kmain`'s keyboard/console loop into a `shell_thread_entry(void)`. `con`,
   `mount`, and `shell_ctx` become file-scope statics in `kernel.c` (or move into a struct
   the shell owns).
2. `create_proc(true)` + `create_thread(p, (uintptr_t)shell_thread_entry,
   THREAD_F_MAIN_THREAD, 0)`. No `+ KERNEL_IMAGE_BASE` on the entry point.
3. `scheduler_set_idle_thread(0, shell_thread)` — **not** `scheduler_add_ready`. The idle
   thread is `pick_next`'s fallback and must stay out of the ready queue.
4. `kmain` ends with `restore_context(shell_thread->context)`.
5. Delete `shell_exec_binary`'s `scheduler_pick_next` / `restore_context`.
   `elf_load_process` already enqueues the new thread and the timer will preempt into it.
6. Remove or disable `run_ring3_preemption_test` in `kernel.c` (it never returns).

### Design consequences to think about

- **The shell will starve while a user program runs.** As the idle thread it is only
  picked when the ready queue is *empty*, so a busy user process gets 100% of the CPU and
  the shell never sees a keystroke until that process exits. If you want the shell to stay
  responsive alongside a running program, it needs to be a normal ready-queue thread and
  the idle thread should stay a separate `hlt` loop. Decide which behaviour you actually
  want before wiring it up — "always come back to it" is satisfied either way, but only the
  ready-queue version keeps it interactive.
- **The shell busy-polls** `keyboard_getchar_nonblock`. As an idle thread that means the
  CPU never halts. There is already a `keyboard_wait_queue` and a `SYS_getkey_wait`
  blocking path (`scheduler_block_current` + `scheduler_wake_one` from the keyboard IRQ)
  that would be the better fit.
- **A kernel thread needs `user_stack_size = 0`** and a proc created with
  `create_proc(true)`, or `create_thread` will reject it.

---

## Known-outstanding issues (not blocking, found while reading)

- `elf_map_segments` at `loader.c` dereferences `t->owning_proc->adress_space` **before**
  the NULL check below it, and never checks `t` at all.
- `elf_load_process` leaks on every failure path after `create_proc`: the proc, its open
  fd, the thread and both stack regions. `mem_file *file` leaks on **all** paths including
  success. A partial cleanup helper was drafted but not applied — `destroy_proc` refuses
  while `thread_head` is set, and a `NEW_T` thread cannot be torn down (`thread_exit`
  requires `RUNNING_T`), so post-`create_thread` failures have no teardown path at all.
- `fat32_file_open` return value is unchecked in `elf_load_process` when populating the
  fd table; on failure it still marks the slot `in_use`.
- `sys_exit` in `syscall.c` dereferences `cpu->current_thread` without a NULL guard.
- `scheduler_pick_next`'s idle fallback branch returns the thread without setting
  `cpu->current_thread`, unlike the normal path. `scheduler_exit_current` compensates with
  its own `scheduler_set_current`; `timer_handler` does not. Worth making the two branches
  consistent.
- `.bss` handling: when `p_memsz > p_filesz`, a tail page still inside the file's byte
  range gets file bytes where zeros belong. `fs_mmap_read` only zero-fills past *file* end,
  not past `filesz`. Not triggered by the current test binaries. `RING3M.EXE`
  (`elf_ring3_mmap_test.S`) is the binary built to exercise exactly this, but it still
  spins instead of exiting and was reported as using the wrong syscalls.
- The comment at `context_switch.asm` `.build_kernel_iret_frame` still claims the CPL0
  return "consumes only RIP, CS, and RFLAGS", which now contradicts the five pushes below
  it.
- `create_vm_space` hardcodes `is_kernel = false` for every space. `reserve_region`'s
  user/kernel VA clamp keys off that flag, so it currently works only because no space is
  ever marked kernel.

---

## Test assets

- `elf_ring3_test.S` → `/RING3.EXE` — 8 rounds of busy-wait + `SYS_write("[RING3-A] round")`,
  then `SYS_exit(0)`.
- `elf_ring3_preempt_test.S` → `/RING3P.EXE` — identical shape, prints `[RING3-B] round`.
  Created this session; `.ld` and the four Makefile hooks are in place.
- `elf_ring3_stack_test.S` → `/RING3S.EXE` — exercises the user stack. Still spins instead
  of exiting.
- `elf_ring3_mmap_test.S` → `/RING3M.EXE` — demand-paging / `.bss` test. Still spins;
  syscall usage needs review.

`run_ring3_preemption_test` in `kernel.c` currently loads RING3.EXE + RING3P.EXE and
contains `probe_thread` / `probe_hex` debug dumps of every thread's stack and context —
useful, and worth keeping around behind a flag rather than deleting outright.

### Running headless

```
cd src/boot && make
qemu-system-x86_64 -machine pc \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=src/boot/build/OVMF_VARS_4M.fd \
  -boot order=d -cdrom secureos-uefi.iso \
  -drive file=src/boot/build/secureos.fat32.img,format=raw,if=ide,index=0,media=disk \
  -serial file:serial.log -display none -m 2048 \
  -d int,cpu_reset -no-reboot -D qemu.log
```

`-d int,cpu_reset -no-reboot` is what made the triple fault findable: the last
`check_exception` chain plus the register dump gives RIP, RSP, CR2 and CR3 at the fault.
Resolve the faulting RIP with
`nm -n src/boot/build/secureos.elf` (subtract `0xFFFFFFFF80000000`).

Note the FAT32 image target must be built by its **absolute** path — `$(FAT32_IMAGE)`
expands to `$(BUILD_DIR)/secureos.fat32.img` with `BUILD_DIR` absolute, so
`make build/secureos.fat32.img` does not match. It is also not a prerequisite of `all`.
