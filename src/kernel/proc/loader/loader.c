#include <loader.h>
#include <pmm.h>
#include <string.h>
#include <scheduler.h>

// Reads exactly len bytes from a byte offset in an open file.
//
// fat32_file_read() reads from the handle's current position, so every caller
// seeks first: the loader jumps around the ELF (the header, then one program
// header at a time) rather than walking it front to back.
//
// A short read is a failure, not a partial success - every caller here is
// reading one fixed-size structure and has nothing to do with half of one.
static bool elf_read_at(fat32_file_t *file, uint64_t offset, void *dst, uint32_t len) {

    if (file == NULL || dst == NULL || len == 0) { return false; }

    // fat32_file_seek() takes a uint32_t; FAT32 cannot address past 4 GiB
    // anyway, and file_size is a uint32_t for the same reason.
    if (offset > UINT32_MAX) { return false; }

    if (fat32_file_seek(file, (uint32_t)offset) != 0) { return false; }

    int read_result = fat32_file_read(file, (uint8_t*)dst, len);

    return read_result == (int)len;
}

void elf_close_image(elf_image_t *image) {

    if (image == NULL) { return; }
    if (image->file.mount == NULL) { return; }

    (void)fat32_file_close(&image->file);
    image->file.mount = NULL;
}

// Opens filename and reads just its ELF header, validating everything the
// mapping step later relies on.
//
// Nothing else about the file is read here, and nothing is copied into kernel
// memory beyond this one 64-byte header. That is the point: a process's text
// and data belong in the process's address space, reached through the
// file-backed mmap() regions elf_map_segments() sets up, not on the kernel
// heap. The previous version of this loader did the opposite - it
// kmalloc()'d file_size and read the whole executable in - which put a hard
// 4096-byte ceiling on executables, because kmalloc() serves kernel objects
// out of the slab allocator and refuses anything larger (heap.c). See
// RING3B.EXE (elf_ring3_big_test.S), which exists to exercise this path.
//
// Validation is deliberately strict, because elf_map_segments() maps segments
// at their p_vaddr with no relocation step:
//   - 64-bit class and ET_EXEC, since nothing here can relocate a PIE or
//     resolve a dynamic segment;
//   - a non-zero entry point, which is also what elf_load_process() reports as
//     "not an ELF";
//   - a program header table that is present, entry-size-compatible, and
//     entirely inside the file, so the per-phdr reads during mapping cannot
//     run off the end.
elf_open_out elf_open_image(fat32_mount_t *mount, const char *filename, elf_image_t *out_image) {

    elf_open_out result = {0};

    if (mount == NULL || filename == NULL || out_image == NULL) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    elf_image_t image = {0};

    if (fat32_file_open(mount, filename, &image.file) != 0) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    image.file_size = image.file.entry.file_size;

    if (image.file_size < sizeof(elf_eheader_t)) {
        elf_close_image(&image);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    if (!elf_read_at(&image.file, 0, &image.header, sizeof(elf_eheader_t))) {
        elf_close_image(&image);
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    const unsigned char *ident = image.header.e_ident;

    if (ident[0] != 0x7F || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F' ||
        ident[EI_CLASS] != ELFCLASS64 ||
        image.header.e_type != ET_EXEC ||
        image.header.e_entry == 0 ||
        image.header.e_phnum == 0 ||
        image.header.e_phentsize < sizeof(elf_pheader_t)) {

        elf_close_image(&image);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    uint64_t phdr_table_bytes = (uint64_t)image.header.e_phnum * image.header.e_phentsize;

    if (image.header.e_phoff > image.file_size ||
        phdr_table_bytes > (uint64_t)image.file_size - image.header.e_phoff) {

        elf_close_image(&image);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    *out_image = image;
    result.success = true;
    return result;
}

// Unwinds a proc that create_proc() handed back but that never reached a
// runnable state: closes anything the loader put in its fd table, then walks
// the normal exit/reap/destroy chain.
//
// Only legal while the proc still owns no threads. destroy_proc() refuses with
// THREAD_ACTIVE once thread_head is set, and a NEW_T thread cannot be torn
// down at all yet - thread_exit() requires RUNNING_T - so a caller that has
// already passed create_thread() has nothing to call and leaks. See TODO.md.
static void release_threadless_proc(proc *p) {

    if (p == NULL) { return; }

    for (int fd = PROC_FD_BASE; fd < PROC_MAX_OPEN_FILES; fd++) {
        if (p->fd_table[fd].in_use &&
            (p->fd_table[fd].kind == PROC_FD_FILE ||
             p->fd_table[fd].kind == PROC_FD_EXEC_IMAGE)) {
            (void)fat32_file_close(&p->fd_table[fd].file);
            p->fd_table[fd].in_use = 0;
            p->fd_table[fd].kind = PROC_FD_NONE;
        }
    }

    if (p->thread_head != NULL) { return; }

    if (!proc_exit(p).success) { return; }
    if (!reap_proc(p).success) { return; }

    (void)destroy_proc(p);
}

elf_map_out elf_map_segments(thread *t, elf_image_t *image) {

    elf_map_out result = {0};

    if (t == NULL || t->owning_proc == NULL || image == NULL) {
        result.error_code = ELF_MAP_INVALID_ARGS;
        return result;
    }

    vm_space *space = t->owning_proc->adress_space;

    if (space == NULL) {
        result.error_code = ELF_MAP_INVALID_ARGS;
        return result;
    }

    elf_eheader_t *header = &image->header;

    // elf_open_image() already checked e_phentsize and that the whole table
    // lies inside the file. Re-checking e_phentsize here is what makes the
    // per-entry seek below safe on its own terms, since this function is
    // callable with any image.
    if (header->e_phentsize < sizeof(elf_pheader_t)) {
        result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
        return result;
    }

    uint64_t phdr_table_bytes = (uint64_t)header->e_phnum * header->e_phentsize;
    if (header->e_phoff > image->file_size ||
        phdr_table_bytes > (uint64_t)image->file_size - header->e_phoff) {
        result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
        return result;
    }

    for (uint16_t i = 0; i < header->e_phnum; i++) {

        // One program header at a time, straight off disk. Reading the table
        // into a buffer would mean sizing that buffer from e_phnum, which is
        // attacker-controlled; this way the loader's memory use is a single
        // 56-byte stack structure no matter what the file claims.
        //
        // Only sizeof(elf_pheader_t) bytes are read even when e_phentsize is
        // larger: the seek accounts for the real stride, and the trailing
        // bytes of an oversized entry are not fields this kernel knows.
        elf_pheader_t phdr_storage = {0};
        elf_pheader_t *phdr = &phdr_storage;

        uint64_t phdr_offset = header->e_phoff + (uint64_t)i * header->e_phentsize;

        if (!elf_read_at(&image->file, phdr_offset, phdr, sizeof(elf_pheader_t))) {
            result.error_code = ELF_MAP_PHDR_READ_FAILED;
            return result;
        }

        if (phdr->p_type != PT_LOAD) { continue; }

        // An empty PT_LOAD is legal and maps nothing. ld emits one routinely:
        // a linker script that names a second RW segment produces it with
        // p_vaddr, p_filesz and p_memsz all zero whenever the binary turns out
        // to have no .data and no .bss to put in it.
        //
        // Skipping is not a nicety - mmap() rejects a zero length, so without
        // this the whole load fails with ELF_MAP_RESERVE_FAILED on a perfectly
        // good executable, and the reported error points at the mapping code
        // rather than at the empty segment.
        if (phdr->p_memsz == 0) { continue; }

        if (phdr->p_offset > image->file_size ||
            phdr->p_filesz > (uint64_t)image->file_size - phdr->p_offset) {
            result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
            return result;
        }

        // US is required on every leaf PTE for a user-mode segment -
        // nothing else in vmm_map_page enforces it.
        uint64_t perms = PTE_US;
        if (phdr->p_flags & PF_W) { perms |= PTE_RW; }
        if (!(phdr->p_flags & PF_X)) { perms |= PTE_NX; } // W^X for non-exec segments

        uintptr_t seg_start = phdr->p_vaddr & ~0xFFFULL;
        uintptr_t seg_end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, 4096);

        // seg_start rounded p_vaddr down by page_slack bytes, so the file
        // offset that belongs at seg_start is p_offset backed up by the same
        // amount. A loadable segment must satisfy p_offset == p_vaddr (mod
        // 4096) - without that congruence the subtraction can underflow and
        // wrap to a ~2^64 offset, so reject the phdr rather than rely on
        // mmap()'s alignment check to catch the wrapped value.
        uintptr_t page_slack = phdr->p_vaddr & 0xFFFULL;
        if ((phdr->p_offset & 0xFFFULL) != page_slack) {
            result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
            return result;
        }

        uint64_t seg_file_offset = phdr->p_offset - page_slack;

        // Reserve the segment through mmap() rather than hand-rolling a
        // vm_object and calling reserve_region directly - one owner for the
        // region bookkeeping, and mmap() already frees the object on failure.
        //
        // The segment is file-backed, not anonymous: mmap() only reserves, so
        // the pages arrive empty and the #PF handler fills them on demand.
        // populate_fault_frame() can only read the ELF's bytes back if the
        // region carries a FILE object, which is what passing a real fd here
        // buys. An anonymous region would zero-fill instead and the process
        // would execute a blank page.
        //
        // reserve_region() is what keeps p_vaddr honest: a non-kernel space
        // clamps FIXED reservations to [0, USER_SPACE_LIMIT_), so a phdr
        // claiming a higher-half VA is rejected there rather than here.
        //
        // The thread is passed explicitly rather than resolved from the
        // scheduler: t was created moments ago and is not the current thread,
        // and on the first process it is not on any run queue yet either.
        // Anything that asked the scheduler here would map into whatever
        // happened to be running instead.
        void *mapped = mmap(t, (void*)seg_start, (seg_end - seg_start), perms,
                            PRIVATE | FIXED, PROC_FD_BASE, seg_file_offset);

        if (mapped == NULL) {
            result.error_code = ELF_MAP_RESERVE_FAILED;
            return result;
        }

        // mmap() hands back the VA, not the region. Look the region back up so
        // the ELF_SEGMENT tag and the teardown path below still work.
        search_region_list_out seg_search = search_region_list(space, (uintptr_t)mapped);
        vm_region *seg_region = seg_search.success ? seg_search.region : NULL;
        if (seg_region == NULL) {
            result.error_code = ELF_MAP_RESERVE_FAILED;
            return result;
        }

        seg_region->type = ELF_SEGMENT;

        // The region spans p_memsz, but only p_filesz bytes of it exist in the
        // file. Everything past that is .bss and must read as zero.
        //
        // The count is measured from the start of the region, which mmap()
        // placed at seg_start - p_vaddr rounded down to a page - so the
        // page_slack bytes preceding p_vaddr are file content too and belong
        // in the total. They are the tail of the previous segment, or the ELF
        // header itself for the first one; either way they are real bytes on
        // disk and the region legitimately maps them.
        //
        // This is why a PT_LOAD does not need splitting into a file-backed
        // region plus an anonymous one: a single file-backed region with an
        // honest valid length already zero-fills its tail, and the partial
        // page where .data ends and .bss begins is handled by the same clamp
        // rather than by a boundary between two regions.
        seg_region->file_valid_len = (size_t)(page_slack + phdr->p_filesz);

     
    }

    result.success = true;
    return result;
}

elf_load_out elf_load_process(fat32_mount_t *mount, const char *filename) {

    elf_load_out result = {0};

    if (mount == NULL || filename == NULL) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    // Header only - the executable's bytes are never copied into the kernel.
    elf_image_t image = {0};
    elf_open_out open_md = elf_open_image(mount, filename, &image);
    if (!open_md.success) {
        result.error_code = open_md.error_code;
        return result;
    }

    create_proc_out proc_md = create_proc(false);
    if (!proc_md.success) {
        elf_close_image(&image);
        result.error_code = ELF_LOAD_PROC_CREATE_FAILED;
        return result;
    }

    // A second, independent handle on the same file, this one owned by the
    // process and living in its fd table. It is what the file-backed regions
    // are pinned to, so it has to outlive this function - the #PF handler
    // reads through it for the whole life of the process, long after the
    // loader's own handle is gone. The two handles are separate because they
    // have separate lifetimes, not because the file is opened twice for its
    // own sake.
    int slot = PROC_FD_BASE;
    if (fat32_file_open(mount, filename, &proc_md.proc->fd_table[slot].file) != 0) {
        release_threadless_proc(proc_md.proc);
        elf_close_image(&image);
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }
    proc_md.proc->fd_table[slot].in_use = 1;
    proc_md.proc->fd_table[slot].kind   = PROC_FD_EXEC_IMAGE;

    // 1 MiB of user stack, up from 64 KiB.
    //
    // 64 KiB was enough for hand-written assembly that never nests. It is not
    // enough for compiled C: newlib's stdio carries large frames, and doom's
    // renderer recurses through BSP traversal. Overflowing runs off the bottom
    // of the reservation into unmapped space, which surfaces as a page fault
    // at an address nowhere near the code that caused it.
    //
    // The cost of asking for more is a larger reservation, not more memory:
    // the region is GROWDOWN and demand-paged, so pages that are never touched
    // are never allocated.
    create_thread_out thread_md = create_thread(proc_md.proc, image.header.e_entry, THREAD_F_MAIN_THREAD, USER_STACK_SIZE);
    if (!thread_md.success) {
        release_threadless_proc(proc_md.proc);
        elf_close_image(&image);
        result.error_code = ELF_LOAD_THREAD_CREATE_FAILED;
        return result;
    }

    elf_map_out map_md = elf_map_segments(thread_md.thread, &image);

    // The loader's handle has done its job either way: program headers are
    // only read during mapping. The process's own fd stays open.
    elf_close_image(&image);

    // From here on the proc owns a thread, and release_threadless_proc()
    // refuses those - a NEW_T thread cannot be torn down yet (see its comment
    // and TODO.md), so a failure past this point still leaks the proc.
    if (!map_md.success) {
        result.error_code = ELF_LOAD_MAP_FAILED;
        return result;
    }

    // Reserve the process's heap arena, which SYS_brk hands out from.
    //
    // Done here rather than lazily on the first brk() call because mmap()
    // needs a thread to attach the region to, and this is where one exists.
    // The reservation is anonymous and demand-paged, so 64 MiB costs a single
    // vm_region and not one byte of physical memory until the program touches
    // something. That is what lets brk() be a bounds check instead of a
    // mapping operation, and why SYS_munmap is not needed to support malloc.
    //
    // USER_BRK_BASE sits far above the link address the userland linker script
    // uses (0x2000000) and far below the stack, which reserve_region() places
    // top-down from USER_SPACE_LIMIT_.
    //
    // A failure here is not fatal: a program that never calls malloc runs
    // fine without a heap, and SYS_brk reports the failure to anything that
    // does. Leaving the three fields zero is what marks the arena absent.
    void *brk_area = mmap(thread_md.thread, (void*)USER_BRK_BASE, USER_BRK_SIZE,
                          PTE_US | PTE_RW | PTE_NX, PRIVATE | FIXED, 0, 0);

    if (brk_area != NULL) {
        proc_md.proc->brk_base    = (uintptr_t)brk_area;
        proc_md.proc->brk_current = (uintptr_t)brk_area;
        proc_md.proc->brk_limit   = (uintptr_t)brk_area + USER_BRK_SIZE;
    }

    sched_add_ready_out ready_md = scheduler_add_ready(0, thread_md.thread);
    if (!ready_md.success) {
        result.success = false;
        result.error_code = ELF_LOAD_SCHEDULER_FAILED;
        return result;
    }

    result.success = true;
    result.p = proc_md.proc;
    result.t = thread_md.thread;
    return result;
}
