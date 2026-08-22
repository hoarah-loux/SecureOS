# vmm.c fixes (2026-08-23)

Six changes, all in `src/kernel/mm/alloc/vmm.c`. Found while debugging the
page fault after the low-identity unmap landed.

## 1. `the_walking_pager` leaked flag bits into physical addresses

The three leaf extractions used `entry & ~((1ULL << N) - 1)`, which clears the
low offset bits but leaves NX (63) and the available bits 62:52 in the result.
Any query against an NX page returned `0x8000_0000_xxxx_xxxx`.

Now masks the real 52-bit address field, same as `the_walking_pager2`'s
`ADDR_MASK_1G/2M/4K`.

This is why `vmm_query_page(&_kernel_end - 1)` came back wrong and the
low-identity unmap range computed as `0x8000000001C00000` bytes. The clamp
caught it, so the unmap covered the right window by accident. That
`[VMM] low identity unmap clamped` line is gone now — the clamp is still
worth keeping as a guard.

## 2. Same bug one level up, in the intermediate descriptors

Nine sites used `& ~0xFFF` to turn a PML4E/PDPTE/PDE into a table pointer:
`vmm_init`, `the_walking_pager` ×3, `populate2m_into_table` ×2,
`populate4k_into_table` ×3. Same leak, and these get dereferenced directly.

Added file-scope `PAGE_TABLE_ADDR_MASK` and applied it to all nine. Latent
rather than live — every caller runs pre-transition where identity mapping
still exists — but it would have been a wild read the moment an intermediate
entry carried NX.

## 3. Directories were created read-only (this one was live)

The CPU ANDs R/W, U/S and NX across *every* level of the walk, not just the
leaf. `spilt_page` built new directories by copying flags from the parent
entry above — but for the first mapping in a fresh address space the PML4 slot
is empty, so there is no parent, and `the_walking_pager2` was returning
`parent_flags = 0` for that case (its PML4 branch never assigned the
`parent_*` fields, unlike the PDPT and PD branches). The code then OR-ed in
only PRESENT and US, never RW. Each level re-read the entry the previous
iteration had just written, so read-only propagated all the way down.

`vmm_map_page` writes the caller's flags into the leaf only and never revisits
the directories, so the PTE looked correct while the page was unwritable. With
CR0.WP set this denied kernel writes too.

Two changes:

- `the_walking_pager2`'s PML4-not-present branch now fills in
  `parent_flags`/`parent_entry`/`parent_table_ptr`/`parent_lvl`
  (`parent_lvl = 0` = "this is the root").
- New `directory_entry_flags(inherited, allow_user)` helper, used by both
  `spilt_page` paths. Policy: **directories stay permissive, the leaf PTE is
  authoritative.** Forces P + RW, forces US for low-half VAs only, clears PS
  and large-page PAT (this entry points at a table, not a page), clears NX.

Nothing had noticed because RING3.EXE's pages are read+execute. Kernel-half
mappings were unaffected — those PML4 slots were already populated by
`poplulate_phys_map`, so the walk found real parent flags.

## 4. NX and U/S were inherited onto directories

Fallout from the same helper, worth calling out separately:

- The NOT_PRESENT path inherited the parent's NX, pinning an entire subtree
  non-executable with no way for a later leaf to opt back in.
- The LARGE_PAGE path carried the old leaf's U/S and NX onto the entry that
  replaced it, so a kernel-only or NX large page could never be split to host
  a user or executable 4 KiB page. Those bits already live on the 512 children
  built from `leaf_flags`, so keeping them on the directory was pure veto.

Both fixed by routing through `directory_entry_flags()`.

## 5. User address space ceiling was in the kernel half

`reserve_region` used `HIGHER_HALF_BASE_` (`0xFFFF800000000000`) as the user
ceiling. That is the bottom of the *kernel* half; the canonical low half ends
at 2^47. TOPDOWN therefore gave the user stack `va_end = 0xFFFF800000000000`,
and `create_thread` uses a region's exclusive `va_end` verbatim as the initial
user RSP — so the first push targeted `0xFFFF7FFFFFFFFFF8` and took #GP before
any page walk.

Added `USER_SPACE_LIMIT_` = `0x00007FFFFFFFF000` (one guard page below 2^47,
so `va_end` is itself canonical) and used it for `legal_limit` in
`reserve_region` and `user_limit` in `create_vm_space`.

## 6. Leaked page-table frames on four `spilt_page` bailouts

`final_entry_pointer == NULL`, `final_page_walked_ptr == NULL` (×2) and
`PARENT_PA_IS_0` all return after allocating a paging-structure frame but
before anything links it into a table, so teardown could never find it. Added
`pmm_free_frame()` to each.

Deliberately **not** added to the two `REWALK_FAILED` returns — the frame is
already installed in the page table there, so freeing it would be a
use-after-free, not a leak fix.

---

## Verified

New `elf_ring3_stack_test.S` builds to `/RING3S.EXE` on the FAT32 image. It
does a push/pop, a call frame with locals, and an 8 KiB frame that crosses a
page, checking each readback. Output:

```
ring3 stack: push/pop, frame, page-cross all OK
```

with exactly two page faults, both `e=0006` (user + write + not-present), both
serviced, zero panics. Before these fixes that first push took #GP into a
vector with no gate — silent triple fault. With only fix 5, the retry after
the demand fault would have hit `e=0007` and panicked on the read-only
directories from fix 3.

`kernel.c` still loads `/RING3.EXE`, so to run the stack test change the path
at `kernel.c:638`.

Baseline unchanged: 39 PASS, 2 FAIL (both pre-existing — `run_syscall_tests`
executes before any process exists, so `get_p()` returns NULL and every fd
syscall bails), 0 faults, 0 panics.

## Still open in vmm.c

`copy_mapping_dynamic` passes the raw entry (address *and* flags) through a
parameter named `physical_adress`, and the callee writes `physical_adress |
pd_e_flags`. It works only because the source entry already carries PS —
`populate2m_into_table` never sets PS itself. Net effect is the destination
leaf gets `source_flags | parent_flags` OR-ed together, so a copied mapping
can silently gain RW or US it did not have. Load-bearing, so it can't just be
masked without also setting PS explicitly.
