# Open Bug — User Stack Top Is Non-Canonical (2026-08-23)

**Status:** diagnosed, not yet applied. Latent — it does not break `RING3.EXE`
today, but it breaks the first user program that touches its stack.

---

## Symptom

Once the scheduler hands off to a ring3 thread, QEMU shows the thread running
with:

```
v=20 e=0000 i=0 cpl=3 IP=001b:00000000004000ce SP=0023:ffff800000000000
```

`SP = 0xffff800000000000`. That is the *first kernel address*, handed to a
ring-3 thread as its stack pointer.

`RING3.EXE` survives only because `elf_ring3_test.S` never pushes anything —
it does one `int 0x80` and then spins. Any program with a `call`, a `push`, or
a stack-allocated local dies on its first instruction that touches the stack.

## Root cause

`reserve_region()` uses the wrong ceiling for user address space
(`src/kernel/mm/alloc/vmm.c:1967`):

```c
if(space->is_kernel == false){
    legal_limit = HIGHER_HALF_BASE_;   // 0xFFFF800000000000
    legal_base = 0;
}
```

`HIGHER_HALF_BASE_` is the **bottom of the kernel half** (PML4 slot 256), not
the top of the user half. On x86-64 with 48-bit virtual addresses the
canonical low half ends at 2^47 = `0x0000800000000000`; everything from there
up to `0xFFFF7FFFFFFFFFFF` is the non-canonical hole. Using the kernel base as
the user ceiling lets user regions be placed inside — and above — that hole.

The chain from there:

1. `reserve_region()`'s TOPDOWN path sets `va_end = legal_limit` for the first
   region in an empty space, so the user stack gets
   `va_end = 0xFFFF800000000000`.
2. `create_thread()` sets the initial user stack pointer straight from that
   exclusive bound (`src/kernel/proc/ds/t.c:79`):
   `t->_rsp_u = u_stack_va_end;`
3. `create_thread()` copies it into the context (`t.c:210`):
   `c->user_rsp = t->_rsp_u;`
4. `restore_context` pushes it as the IRETQ frame's RSP
   (`src/arch/x86_64/context_switch.asm:88`), and the thread enters ring3 with
   `RSP = 0xFFFF800000000000`.

`IRETQ` accepts it — the value itself is canonical, it is just a kernel
address. The failure comes on the first `push`, which computes
`RSP - 8 = 0xFFFF7FFFFFFFFFF8`. That address *is* non-canonical, so the CPU
raises `#GP`/`#SS` before any page walk happens. A page-fault handler never
gets a chance to demand-back it.

## Fix

Add the correct user ceiling next to `HIGHER_HALF_BASE_`
(`src/kernel/mm/alloc/vmm.c:16`):

```c
#define HIGHER_HALF_BASE_ 0xFFFF800000000000ULL // slot 256 in the pml4

// Top of the canonical low half is 2^47 - at or above that is the
// non-canonical hole, not kernel space. One guard page below it so that a
// region's exclusive va_end - which create_thread() uses verbatim as the
// initial user RSP - is itself a canonical address.
#define USER_SPACE_LIMIT_ 0x00007FFFFFFFF000ULL
```

Then swap the two places that use the wrong constant:

```diff
@@ vmm.c:1908  (create_vm_space)
-    new_space->user_limit = HIGHER_HALF_BASE_; //also fill this
+    new_space->user_limit = USER_SPACE_LIMIT_; //also fill this

@@ vmm.c:1967  (reserve_region)
     if(space->is_kernel == false){
-        legal_limit = HIGHER_HALF_BASE_;
+        legal_limit = USER_SPACE_LIMIT_;
         legal_base = 0;
```

Line 1967 is the one that changes behaviour. `space->user_limit` is written
but never read anywhere in the tree, so line 1908 is for consistency only.

### Why the guard page matters

Without the `- 0x1000`, `legal_limit` would be exactly `0x0000800000000000`
and TOPDOWN would produce `va_end = 0x0000800000000000` — which is the *first*
non-canonical address, so `_rsp_u` would be non-canonical from the start. Any
instruction that reads through RSP with a non-negative displacement would
fault, and the value could not even be reloaded into RSP safely. Dropping one
page keeps the exclusive bound itself canonical.

### Result

For a 16 KiB user stack in a fresh address space:

| | before | after |
|---|---|---|
| `va_start` | `0xFFFF7FFFFFFFC000` | `0x00007FFFFFFFB000` |
| `va_end` / `_rsp_u` | `0xFFFF800000000000` | `0x00007FFFFFFFF000` |
| first push writes to | `0xFFFF7FFFFFFFFFF8` (non-canonical, `#GP`) | `0x00007FFFFFFFEFF8` (in region) |

`va_end` is 4096-aligned and therefore also 16-aligned, which satisfies the
System V AMD64 entry requirement for `_start`.

No other change is needed. The first push takes page-fault error code `0x6`
(user, write, not-present), and `page_fault_handler()` already demand-backs
that case (`src/kernel/interrupts/handlers.c:255`) through
`search_region_list()` → `pmm_alloc_frame()` → `populate_fault_frame()` →
`vmm_map_page()`.

## Deliberately not changed

`src/kernel/mm/alloc/vmm.c:1346` also compares against `HIGHER_HALF_BASE_`:

```c
if(virtual_adress < HIGHER_HALF_BASE_){ /* set US bit on intermediate PTEs */ }
```

That one is fine as-is. It is a "does this VA live in the low half" test used
to decide US-bit enforcement on intermediate page-table entries, and
`the_walking_pager2()` rejects non-canonical addresses before this point, so
it cannot misclassify a hole address.

## Related, not fixed here

`legal_base = 0` for user space means a bottom-up reservation can hand out
VA 0, so a null dereference in a user program would land on a mapped page
instead of faulting. A floor of `0x1000` is the usual guard. Left alone
because it is a separate concern from the stack bug.
