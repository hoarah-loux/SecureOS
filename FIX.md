# Fix Report — Scheduler Kickoff / Ring3 Execution (2026-08-22)

## Goal

Wire the scheduler up for real: `scheduler_init()` → build an idle thread →
load `/RING3.EXE` as a process → hand it to the scheduler → `restore_context()`
into it. This had never been done before — `scheduler_init()` existed but was
never called from anywhere in `src/`, and no code had ever built a thread from
a loaded ELF entry point and handed it to the scheduler.

Every bug below was found because this was the **first time** this code path
ever ran for real. All of it compiled and "worked" in isolation before; none
of it had ever been exercised end-to-end under a live CR3 switch into ring3.

Net result: the kernel now genuinely creates a process, switches CR3, and
executes real ring3 instructions — confirmed via GDB register inspection, not
inferred from the absence of a crash. One further bug remains open (documented
at the end) in the `int 0x80` syscall gate, which is why `RING3.EXE` can't yet
print its `hello from ring3` proof string.

---

## 1. `src/kernel/proc/sched/scheduler.c` — idle-thread fallback

**Why:** `scheduler_pick_next()` returned `NULL` whenever the ready queue was
empty — there was no fallback thread to run. The code even had a self-flagged
comment admitting this: `// this needs to be patched so that it runs the idle
thread`. The kickoff was always going to hit this immediately: the only ready
thread (`RING3.EXE`'s main thread) spins forever after its first burst of
instructions, so the ready queue empties on the very first preemption tick and
the scheduler would have silently done nothing from then on.

`scheduler_set_idle_thread()` was also declared in `hdr/scheduler.h` but never
defined anywhere — calling it would have failed to link.

**What changed:**
- Implemented `scheduler_set_idle_thread(cpu_id, t)` — stores `t` on
  `cpu->idle_thread`. Straightforward, mirrors `scheduler_set_current()`.
- `scheduler_add_ready()` now checks `if (t == cpu->idle_thread)` right after
  validating the CPU, and returns success immediately without touching the
  ready queue. The idle thread is never queue-managed — it's always
  implicitly available as the `pick_next` fallback — so every caller that
  routinely re-adds the "current" thread to ready (`timer_handler`,
  `yield_handler`, `scheduler_yield`'s internals, `scheduler_wake_one`) can
  stay generic and not special-case idle.
- `scheduler_pick_next()`: when `thread_queue_pop()` fails (empty queue),
  falls back to `cpu->idle_thread` if one is registered — sets its state to
  `RUNNING_T` and returns it instead of `NULL`.

No other logic changed. `scheduler_exit_current()`'s own "idle thread may be
postponed to init" gap is covered for free since it calls `scheduler_pick_next`
internally.

---

## 2. `src/kernel/proc/sched/context.c` — `save_trap_frame()` heap leak

**Why:** Every single call to `save_trap_frame()` did `kcalloc(1,
sizeof(trap_frame))` and `kcalloc(1, sizeof(context))`, then overwrote
`t->context` with the new pointer — leaking the *old* `context` object every
time, plus leaking the scratch `trap_frame` object outright (its only use was
as an intermediate copy to populate `context_save`; both callers in
`handlers.c` — `timer_handler` and `yield_handler` — ignore
`result.frame_saved` entirely). The function's own comment already admitted
this: `// we are leaking memory here need to alloc to the already exiositing
thing`.

This was not yet a real problem before this round because nothing had ever
actually driven the scheduler under live timer preemption. It is a real
problem the moment the kickoff runs: the timer fires at 100Hz with a 5-tick
quantum, so `save_trap_frame()` would be called roughly 20 times a second per
thread being preempted, leaking two heap objects each time. Left unfixed, the
heap would have been exhausted within seconds to minutes of the scheduler
actually running — which is exactly what this round needed to prove works.
Fixed proactively, before it was ever hit, rather than after a heap-exhaustion
crash.

**What changed:**
- `context *context_save = t->context;` — reuse the thread's own,
  already-allocated `context` object in place instead of allocating a new
  one. `t->context` is guaranteed non-NULL for any thread built by
  `create_thread()`, so a NULL guard (`t->context == NULL`) was added to the
  top instead of trusting that invariant silently.
- Removed the `trap_frame *frame_save` scratch object and its full field-by-
  field copy entirely. Every field that used to be copied `frame → frame_save
  → context_save` (a pointless double-hop) is now copied directly `frame →
  context_save`.
- `result.frame_saved` is now explicitly set to `NULL` (documented in a
  comment: no longer allocated, unused by any current caller) instead of
  pointing at a since-removed allocation.
- The redundant `t->context = context_save;` line was dropped — it's already
  the same pointer, the assignment did nothing.

No caller-visible behavior changed (same struct populated, same success/error
semantics); only the allocation pattern changed.

---

## 3. `src/kernel/mm/alloc/pmm.c` — `BUDDY_MAX_PHYS` included non-RAM regions

**Why:** This was the first of two bugs that only became visible once
`create_vm_space()` started actually placing a *new kind* of virtual address
(the low-identity kernel-image alias, see §5 below) — before that, nothing
in the system had ever needed to compute a large "how much physical memory do
we manage" figure and use it for VA placement in a way that would expose an
inflated value.

`pmm_init()` parses the multiboot2 memory map in two passes. The **first**
pass computes `pmm_max_phys_end` by taking the highest `base + length` across
**every** memory-map entry, regardless of type — not just usable RAM (type 1).
Under UEFI/OVMF, that memory map includes large reserved/MMIO regions at very
high addresses (a 64-bit PCI MMIO aperture commonly sits up near the 1TiB
range under QEMU's OVMF firmware). So `pmm_max_phys_end` ended up computing to
roughly 1TiB, even though the machine only has 2GB of RAM (`-m 2048`).

The **second** thing `pmm_init()` does with this value is compute
`managed_phys_end = min(pmm_max_phys_end, max_pyhs)` where `max_pyhs` is a
static 4GiB cap — and *that* correctly-clamped value is what actually sizes
the frame bitmap (`pmm_managed_frame_count`/`pmm_managed_bitmap_size`). So the
allocator itself was never at risk of trying to manage 1TiB of frames. The bug
was that `pmm_max_phys_end` — the *raw, unclamped* value — was never itself
updated to the clamped figure, and it's `pmm_max_phys_end` that
`pmm_max_phys_addr()` returns, which is what `BUDDY_MAX_PHYS` (a global in
`vmm.c`) is set from at VMM init.

`BUDDY_MAX_PHYS` in turn feeds `vmm.c`'s computation of `physmap_end =
PHYS_MAP_BASE + align_up(BUDDY_MAX_PHYS, 2MB)` — the address right past the
end of the physmap, which is also the starting point for every
`KERNEL_ASSET`-flagged VA region (kernel stacks, in this codebase). With
`BUDDY_MAX_PHYS` wrongly at ~1TiB instead of ~2-4GiB, the very first kernel
stack ever allocated (for `RING3.EXE`'s own thread) landed at
`PHYS_MAP_BASE + ~1TiB` — a PML4 slot that nothing in the entire system had
ever touched before, two full slots away from the physmap's own slot. Mapping
anything there walked into page-table territory with no existing structure,
and (combined with other bugs below) triple-faulted.

**What changed:**
- Right after computing `managed_phys_end` (the correctly clamped value)
  inside `pmm_init()`'s first pass, added one line:
  `pmm_max_phys_end = managed_phys_end;`
- This makes `pmm_max_phys_addr()` (and therefore `BUDDY_MAX_PHYS`, and
  everything derived from it — physmap sizing, kernel-asset VA placement)
  reflect what the allocator actually manages, instead of the raw high-water
  mark across every firmware-reported region including non-RAM ones.

This is a one-line fix but was found by tracing a triple fault all the way
back through `physmap_end`'s computation — the crash symptom (a fault deep in
page-table code at a PML4 slot the system had never touched) was several
layers removed from the actual root cause.

---

## 4. `src/kernel/mm/alloc/vmm.c` — the big one, four related fixes

This file got the most substantial changes. All four are consequences of one
underlying gap: **`create_vm_space()` never mapped the kernel's own
low-identity address alias into new processes' page tables**, and fixing that
properly required touching code that had never run post-boot before.

### 4a. Background: what `create_vm_space()` was missing

`transition()` (the function that switches the kernel off its bootstrap
page tables onto its real ones) maps the kernel image **twice**: once at its
real, low physical/virtual address (`_kernel_start`..`_kernel_end`, 2MB-
aligned — this is the address the kernel actually executes from, confirmed by
checking every RIP value seen throughout this entire debugging session, all
of which were low addresses, never the higher-half ones), and once at
`KERNEL_IMAGE_BASE` (a higher-half alias that the `-mcmodel=kernel` compile
flags and constants imply is the "real" execution address, but which turns
out not to be the one actually in use).

`create_vm_space()` already copied the kernel's higher-half PML4 entries
(slots `[256, 511]`, covering the physmap and the higher-half kernel-image
alias) into every new process's page table, by copying the raw 8-byte PML4
*pointers* — since those slots are meant to be identical and shared across
every address space, sharing the underlying PDPT/PD/PT structures by pointer
is correct and cheap.

It never did the same for the **low**-identity alias, because that alias
lives in PML4 slot 0 — the exact same slot every process's own low/user
address space (stack, heap, ELF segments) lives in. You can't share that slot
wholesale the way the higher-half ones are shared, because doing so would
alias the kernel's own low mapping directly on top of (or overlapping with)
whatever a specific process's user-space mappings need to occupy in that same
slot. So it was simply never copied at all — meaning that as soon as any new
process's CR3 went live, the kernel had no page to keep executing from, and
the very next instruction after the CR3 switch would fault.

### 4b. Fix: replicate the kernel-image mapping per-process

In `create_vm_space()`, right after the existing higher-half copy loop, added:

```c
uintptr_t ki_src_start_va = ((uintptr_t)&_kernel_start) & ~((uintptr_t)0x1FFFFF);
uintptr_t ki_src_end_va = (((uintptr_t)&_kernel_end) + 0x1FFFFF) & ~((uintptr_t)0x1FFFFF);
if(!copy_mapping_dynamic_over_range(kernel_root, ki_src_start_va, ki_src_end_va,
                                     new_pml4.pgtable_pointer, ki_src_start_va)){
    kfree(new_space);
    pmm_free_frame(new_pml4.physical_adress);
    result.error_code = PML4_ALLOC_FAIL;
    return result;
}
```

This reuses `copy_mapping_dynamic_over_range()` — the exact same helper
`transition()` itself uses to build this same mapping in the very first
place — to replicate just the kernel image's own mapped range into the new
process's table, instead of trying to share a whole PML4 slot.

### 4c. Fallout: the copy helper chain had never run post-transition, and had the same physical/virtual bug class as an earlier round's buddy-allocator fixes

`copy_mapping_dynamic_over_range()` calls `copy_mapping_dynamic()`, which
calls `populate4k_into_table()` / `populate2m_into_table()`, which call
`the_walking_pager()` (note: the non-`"2"` version) and
`free_frame_for_new_page_structure_and_create_it()` (also the non-`"2"`
version). Every one of these functions had, until this round, **only ever
been called from inside `transition()` itself**, which runs *before*
`physmap_ready` is set — i.e., before the physmap even exists, back when the
early boot page tables still identity-map the full low address range.

Because of that, none of them ever translated a physical intermediate-table
address (a PDPT/PD/PT pointer read out of a page-table entry — which is
*always* physical, that's just how x86 paging entries work) through
`phys_to_virt()` before dereferencing it. Casting it straight to a pointer and
reading through it is only correct when physical and virtual addresses are
numerically identical, which is true pre-transition (full identity map) and
false afterward (only the physmap and the kernel-image slivers are mapped).
This is the exact same bug class an earlier round of this session already
found and fixed in `buddy.c` (`node_kva()`/`fm_base()`), just in a completely
different part of the codebase that had never been exercised long enough to
expose it.

Calling `create_vm_space()`'s new kernel-image copy (§4b) — which runs well
after `transition()`, with `physmap_ready == true` — walked straight into
this: `the_walking_pager()` dereferenced a physical PDPT address as if it were
already virtual and faulted.

**What changed** — added one small helper, phase-gated the same way
`node_kva()`/`fm_base()` are in `buddy.c`:

```c
static inline uintptr_t *pt_kva(uintptr_t phys_table_addr) {
    if (!physmap_ready) { return (uintptr_t *)phys_table_addr; }
    return (uintptr_t *)phys_to_virt(phys_table_addr);
}
```

Applied it at every raw `(uintptr_t *)(entry & ~0xFFF)`-style cast across four
functions:
- `the_walking_pager()` — the three intermediate-level dereferences (PDPT,
  PD, PT addresses extracted from PML4/PDPT/PD entries respectively).
- `free_frame_for_new_page_structure_and_create_it()` — was casting the raw
  physical frame straight to a pointer and zeroing through it unconditionally
  (`uintptr_t *free_frame_ptr = (uintptr_t *) free_frame;`); now routes
  through `pt_kva()` so it works in both phases, matching what its `"2"`
  sibling (`free_frame_for_new_page_structure_and_create_it2`) already did
  unconditionally (that sibling is only ever called post-transition, so it
  never needed the phase gate).
- `populate4k_into_table()` — three intermediate-level dereferences (PML4→
  PDPT, PDPT→PD, PD→PT descent when an entry already exists).
- `populate2m_into_table()` — two intermediate-level dereferences (PML4→PDPT,
  PDPT→PD).

This was **not** a blanket "wrap everything in `phys_to_virt()`" change (an
earlier round of this session already tried that once elsewhere and it broke
the pre-transition boot path, because `phys_to_virt()` unconditionally adds
`PHYS_MAP_BASE` regardless of whether the physmap actually exists yet). The
gate specifically preserves the old, unsafe-but-correct-for-that-phase
behavior pre-transition (where `transition()` itself still needs these
functions to work exactly as before) and only switches to the translated
behavior once `physmap_ready` is true.

### 4d. Fallout #2: intermediate PML4/PDPT entries silently blocked ring3 access to sibling mappings

Once §4b and §4c together got `create_proc()`/`create_vm_space()` succeeding
again, a *new* symptom appeared: `elf_map_segments()` started failing to map
`RING3.EXE`'s own code segment — not with a page fault, but with
`vmm_map_page()` cleanly reporting `SPILT_ERROR` for the segment's own
already-correct mapping request.

Root cause, found via GDB register inspection at the point of a later
mysterious ring3 instruction-fetch fault (`error_code=0x15`, present +
user-mode + instruction-fetch = an NX-shaped fault, but on a page whose own
leaf PTE was verified — byte for byte — to *not* have the NX bit set): x86-64
evaluates the **US** (user/supervisor) permission bit as the **AND** across
every level of the page-table walk, not just the final leaf PTE. If the
PML4 entry or the PDPT entry covering an address has `US=0`, ring3 code can
never reach anything under it, no matter what the leaf PTE says.

The kernel's own low-identity mapping (correctly `US=0` — it must never be
user-accessible) and `RING3.EXE`'s own segment both live in the same low 1GB
of address space, meaning they share the exact same PML4[0] and PDPT[0]
entries. `populate4k_into_table()`/`populate2m_into_table()` (used by §4b's
copy, which runs *first*, before any ELF segment is ever mapped) create those
shared intermediate entries by blindly inheriting the flags of the *source*
table's own entries — which for the kernel's own mapping are correctly
`US=0`. Nothing afterward ever promoted them to `US=1` when a later,
genuinely user-accessible mapping (the ELF segment) needed to live underneath
that same shared PDPT.

Notably, `spilt_page()` (the function that creates missing intermediate
levels for the *normal*, `vmm_map_page()`-driven lazy-alloc path) already had
exactly the right handling for this — it forces `US=1` on any newly-created
intermediate level when the destination address is below
`HIGHER_HALF_BASE_`. `populate4k_into_table()`/`populate2m_into_table()` (the
older, `copy_mapping_dynamic`-only pair) simply never had the same logic,
because nothing had ever needed them to create a *shared* intermediate
structure before.

**What changed:** in both `populate4k_into_table()` and
`populate2m_into_table()`, added:

```c
uint64_t intermediate_us_bit = (virtual_adress < HIGHER_HALF_BASE_) ? PTE_US : 0;
```

and OR'd `intermediate_us_bit` into every newly-created **intermediate**
(non-leaf) entry write — the PML4→PDPT link, the PDPT→PD link, and (in the 4K
path only) the PD→PT link. The **leaf** entry writes were deliberately left
untouched: the actual kernel-vs-user restriction still needs to be enforced
there (the kernel's own leaf PTEs correctly stay non-user), only the shared
intermediate scaffolding needed to stop being unintentionally restrictive.

---

## 5. `src/kernel/proc/ds/t.c` — kernel stacks can no longer be lazily faulted in

**Why:** Found before the four `vmm.c` bugs above, while first reproducing a
triple fault out of `restore_context()`'s own GPR-push sequence (the very
first time *any* thread was ever actually switched into). Kernel-mode (CPL0)
exceptions do **not** get an automatic stack switch — that mechanism (TSS.RSP0)
only exists for privilege-elevating transitions, i.e. a ring3→ring0 entry.
So if the current, already-CPL0 stack itself is what's unmapped, a `#PF`
delivered while running at CPL0 has no valid stack to build its own exception
frame on — the exception-delivery push immediately faults again on the same
broken stack, which the CPU converts to a `#DF` (double fault), which faults
again the same way and converts to a triple fault (CPU reset).

Kernel stacks in this codebase were being reserved the same lazy way user
stacks are (`reserve_region()` only allocates virtual address space; the
actual physical backing normally happens on first touch, via the page-fault
handler's lazy-alloc path) — which is fine for a **user** stack, because a
ring3 fault *does* get the automatic TSS.RSP0 switch onto a (separately,
correctly mapped) kernel stack, so there's no chicken-and-egg there. It's
specifically the kernel's own stack that can't be lazy, because nothing else
is available to catch the fault that would otherwise populate it.

**What changed:** in `create_thread()`, right after the kernel stack's VA
region is reserved (and before `_rsp_k` is set from it), added a loop that
eagerly backs every page in that region with a real physical frame:

```c
for (uintptr_t page_va = k_stack_va_start; page_va < k_stack_va_end; page_va += 4096) {
    uintptr_t frame = pmm_alloc_frame();
    if (frame == 0) { kfree(t); result.error_code = KERNEL_STACK_RESERVE_ERROR; return result; }

    void *page_kva = (void*)phys_to_virt(frame);
    memset(page_kva, 0, 4096);

    map_out map_md = vmm_map_page(page_va, frame, space->pml4_phys_ptr, 4, KERNEL_STACK_FLAGS);
    if (!map_md.success) { kfree(t); result.error_code = KERNEL_STACK_RESERVE_ERROR; return result; }
}
```

Same pattern already used by `elf_map_segments()` in `loader.c` for eagerly
backing ELF `PT_LOAD` segments (`pmm_alloc_frame` → zero via the physmap alias
→ `vmm_map_page` with the process's physical PML4 root). User stacks were
deliberately left lazy — no chicken-and-egg problem there, no reason to give
up the lazy-allocation benefit.

Two new includes (`<pmm.h>` for `pmm_alloc_frame`, `<string.h>` for `memset`)
were added to `t.c` to support this.

---

## 6. `src/kernel/core/kernel.c` — the actual kickoff

**Why:** This is the change the whole round was about. Nothing previously
called `scheduler_init()`, `scheduler_add_ready()`, `scheduler_pick_next()`,
or `restore_context()` from `kmain` — the entire scheduler subsystem was
linked into the build but never driven.

**What changed:**
- Added `#include <scheduler.h>`.
- Added a trivial idle-thread entry point at file scope: `idle_thread_entry()`
  — an infinite `hlt` loop, used as the idle thread's `rip`.
- Right after the existing `elf_load_process("/RING3.EXE")` test block (still
  logs PASS/FAIL first, unchanged), added the kickoff sequence, gated on the
  load having succeeded:
  1. `scheduler_init()`.
  2. Build an idle kernel process + thread (`create_proc(true)` +
     `create_thread(..., (uintptr_t)idle_thread_entry, THREAD_F_MAIN_THREAD,
     0)`), then `scheduler_set_idle_thread(0, idle_t_md.thread)`.
  3. `scheduler_add_ready(0, load_md.t)` — enqueue `RING3.EXE`'s main thread.
  4. `scheduler_pick_next(0)` + `scheduler_set_current(0, first)` to pick the
     initial thread (which will be `RING3.EXE`'s, since it's the only thing
     in the ready queue at this point).
  5. `restore_context(first->context)` — `noreturn` on success. Every step
     above is checked and `printk`'s a specific failure message if it doesn't
     succeed, so a failure anywhere in the chain falls through to the
     existing console/shell code below instead of hanging silently.

This was a deliberate, scoped decision (confirmed with the user before
implementing): since `restore_context()` never returns on success, `kmain`'s
existing tail — console init and the interactive shell loop — becomes
unreachable once the kickoff succeeds. Threading the shell so it survives
alongside the scheduler was explicitly left for a follow-up round, not
attempted here.

---

## 7. `elf_ring3_test.ld` — relinked from `0x400000` to `0x2000000`

**Why:** Direct consequence of §4b/§4c actually working. Once
`create_vm_space()` correctly maps the kernel's own low-identity range into
every process (as it now does), that range becomes real, present, occupied
address space in every process's own table — and it turns out to be much
bigger than expected: `_kernel_end` is `0x1c00000` (28MB), inflated by the
`-g3` debug-symbol compile flag and no stripping. `RING3.EXE`'s old load
address (`0x400000`, 4MB — the traditional default ELF base, matching what
`elf.ld` also uses) sits well inside that 28MB kernel-image range. Before this
round's fix, that conflict was invisible, because the kernel's low mapping was
never present in any process's table at all. Now that it is, `elf_map_segments`
collided with the kernel's own already-present PTEs at `0x400000`.

**What changed:** the one line setting the segment's base address:

```
. = 0x2000000 + SIZEOF_HEADERS;
```

(`0x2000000` = 32MB, comfortably past `_kernel_end`.) `elf_ring3_test.S` was
checked and needed no changes — it addresses everything via `lea rsi,
[rip+msg]` (RIP-relative), so relinking at a different base doesn't touch its
logic at all. `elf.ld` (the separate, Linux-ABI `/ELF.EXE` test binary used
only for header-parsing tests, never actually mapped/executed) was
deliberately left untouched — it's never passed to `elf_map_segments`, so it
can't collide.

---

## What's verified vs. what's still open

**Verified, via GDB register inspection at the live fault/breakpoint, not
inferred:**
- `create_proc()` → `elf_map_segments()` → `create_thread()` succeed cleanly
  for a real ELF file, building a real process and thread.
- The scheduler kickoff runs: idle thread built, `RING3.EXE`'s thread
  enqueued and picked, `restore_context()` called.
- The CR3 switch is real and correct: after the switch, the CPU is genuinely
  executing at `RING3.EXE`'s real entry point in real ring3 mode (`CS=0x1B`,
  `SS=0x23`, `CPL=3`), and steps through the compiled instructions in the
  correct order (`mov rax,1` / `mov rdi,1` / `lea rsi,[rip+msg]` / `mov
  rdx,18`) before reaching its own `int 0x80`.
- Full existing self-test suite (PMM/VMM/FAT32/syscall/timer) still passes on
  every rebuild throughout this round — none of the fixes above regressed
  anything upstream.

**Not yet fixed, documented in `TODO.md`:** `RING3.EXE`'s own `int 0x80`
(its only way to prove itself, by printing `hello from ring3` via
`SYS_write`) triple-faults instead of ever reaching `syscall_handler` in C.
Diagnosed extensively — `TSS.RSP0` verified correct via GDB byte-level memory
inspection at the exact point of the fault, IDT gate encoding for
`SYSCALL_VECTOR` inspected and matches the already-working `YIELD_VECTOR`/
`EXIT_VECTOR` encoding — but not yet root-caused. This is the first time any
DPL=3 IDT gate has ever been triggered by a genuine `int` instruction from
real ring3 code (every earlier "test" of these vectors called the C handler
directly from `kmain`'s synthetic harness, never a real hardware gate
transition), so it may be a bug that has always been latent in the gate
mechanism itself rather than something this round's changes introduced.

## Files touched this round

```
TODO.md                            — findings + status, documentation only
elf_ring3_test.ld                  — relinked base address (§7)
src/kernel/core/kernel.c           — the kickoff wiring (§6)
src/kernel/mm/alloc/pmm.c          — BUDDY_MAX_PHYS clamp (§3)
src/kernel/mm/alloc/vmm.c          — kernel-image copy + phase-gated walker + US-bit fix (§4)
src/kernel/proc/ds/t.c             — eager kernel-stack backing (§5)
src/kernel/proc/sched/context.c    — save_trap_frame leak fix (§2)
src/kernel/proc/sched/scheduler.c  — idle-thread fallback (§1)
```

No changes were made to `src/kernel/proc/loader/loader.c` or
`src/kernel/interrupts/syscall.c` in the final state — both were used for
temporary debug `printk` instrumentation while diagnosing the bugs above, and
that instrumentation was fully reverted once each bug was found; `git diff`
against both shows no changes.
