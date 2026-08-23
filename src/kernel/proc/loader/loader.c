#include <loader.h>

// Tears down one region of a half-built segment by VA. Best effort - used
// only on the error path, where leaving a stale reservation behind would
// wedge any retry at the same address with FIXED_OVERLAP_ERROR.
static void elf_unmap_va(vm_space *space, uintptr_t va) {

    if (space == NULL) { return; }

    search_region_list_out found = search_region_list(space, va);
    if (!found.success || found.region == NULL) { return; }

    (void)destroy_vm_region(space, found.region);
}

elf_map_out elf_map_segments(vm_space *space, const uint8_t *phdr_table,
                             const elf_eheader_t *header, fs_mmap_backing *backing,
                             uint64_t file_size) {

    elf_map_out result = {0};

    if (space == NULL || phdr_table == NULL || header == NULL || backing == NULL) {
        result.error_code = ELF_MAP_INVALID_ARGS;
        return result;
    }

    if (header->e_phentsize < sizeof(elf_pheader_t)) {
        result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
        return result;
    }

    for (uint16_t i = 0; i < header->e_phnum; i++) {

        const elf_pheader_t *phdr =
            (const elf_pheader_t*)(phdr_table + (uint64_t)i * header->e_phentsize);

        if (phdr->p_type != PT_LOAD) { continue; }
        if (phdr->p_memsz == 0) { continue; }

        // Bounds are checked against the real on-disk size, since that is what
        // the fault handler will read from.
        if (phdr->p_offset > file_size || phdr->p_filesz > file_size - phdr->p_offset) {
            result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
            return result;
        }

        if (phdr->p_filesz > phdr->p_memsz) {
            result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
            return result;
        }

        // US is required on every leaf PTE for a user-mode segment -
        // nothing else in vmm_map_page enforces it. These perms are stored on
        // the region and re-read by the page fault handler for every page it
        // populates, so they apply to demand-faulted pages too.
        uint64_t perms = PTE_US;
        if (phdr->p_flags & PF_W) { perms |= PTE_RW; }
        if (!(phdr->p_flags & PF_X)) { perms |= PTE_NX; } // W^X for non-exec segments

        uintptr_t seg_start = phdr->p_vaddr & ~0xFFFULL;
        uintptr_t seg_end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, 4096);
        uint64_t page_off = phdr->p_vaddr & 0xFFFULL;

        uintptr_t file_region_end = seg_start; // exclusive, page aligned
        bool file_region_mapped = false;

        if (phdr->p_filesz > 0) {

            // Demand paging reads the file at (region base offset + page
            // delta), so a segment's file offset and virtual address have to
            // agree modulo the page size. Every real toolchain emits PT_LOAD
            // that way; refuse rather than silently load shifted bytes.
            if ((phdr->p_offset & 0xFFFULL) != page_off) {
                result.error_code = ELF_MAP_BAD_FILE_ALIGNMENT;
                return result;
            }

            // len counts from the page-aligned segment base, so it includes
            // the leading slack when p_vaddr is not page aligned. Everything
            // past it in the final page is zeroed by the fault handler, which
            // is exactly the .bss tail sharing that page.
            uint64_t file_map_len = page_off + phdr->p_filesz;

            mmap_request file_map = {0};
            file_map.space = space;
            file_map.addr = (void*)seg_start;
            file_map.len = file_map_len;
            file_map.perms = perms;
            file_map.map_flags = PRIVATE | FIXED;
            file_map.backing = backing;
            file_map.file_offset = phdr->p_offset - page_off;
            file_map.region_type = ELF_SEGMENT;

            if (mmap_ex(&file_map) == NULL) {
                result.error_code = ELF_MAP_MMAP_FAILED;
                return result;
            }

            file_region_mapped = true;
            file_region_end = seg_start + ALIGN_UP(file_map_len, 4096);
        }

        // Whatever is left of p_memsz past the last file-backed page is pure
        // .bss: an anonymous mapping, zero filled on fault, no file read.
        if (seg_end > file_region_end) {

            mmap_request bss_map = {0};
            bss_map.space = space;
            bss_map.addr = (void*)file_region_end;
            bss_map.len = seg_end - file_region_end;
            bss_map.perms = perms;
            bss_map.map_flags = PRIVATE | FIXED;
            bss_map.backing = NULL;
            bss_map.file_descriptor = 0;
            bss_map.file_offset = 0;
            bss_map.region_type = ELF_SEGMENT;

            if (mmap_ex(&bss_map) == NULL) {
                if (file_region_mapped) { elf_unmap_va(space, seg_start); }
                result.error_code = ELF_MAP_MMAP_FAILED;
                return result;
            }
        }
    }

    result.success = true;
    return result;
}

// Reads and validates the ELF header straight off the pinned file. The
// loader used to get this out of a whole-file copy in the heap (load_file),
// which put a hard ceiling on the size of a loadable binary: kmalloc refuses
// anything over 4096 bytes, so any executable larger than one page failed to
// load. Now that the segments are demand paged there is no reason to have the
// file in memory at all - only the headers are needed, and they come from the
// same backing the fault handler will use.
static bool elf_read_header(fs_mmap_backing *backing, elf_eheader_t *out) {

    if (backing == NULL || out == NULL) { return false; }

    if (fs_mmap_read(backing, 0, (uint8_t*)out, sizeof(*out)) < 0) { return false; }

    if (out->e_ident[0] != 0x7F || out->e_ident[1] != 'E' ||
        out->e_ident[2] != 'L'  || out->e_ident[3] != 'F') {
        return false;
    }

    return true;
}

// Unwinds a process the loader created but could not finish.
//
// create_proc() hands back a process in NEW with no threads, while
// destroy_proc() only accepts one that is already TERMINATED and reaped - so
// the normal exit path has to be walked first rather than calling
// destroy_proc() directly. Every failure after create_proc() used to just
// return, leaking the proc struct and, worse, its whole address space: a PML4
// plus every page table under it, none of which any later code could find
// again.
//
// Tearing down the address space also destroys the segment regions, which is
// what drops their references on the pinned file.
static void elf_abandon_proc(proc *p) {

    if (p == NULL) { return; }

    (void)proc_exit(p);
    (void)reap_proc(p);
    (void)destroy_proc(p);
}

elf_load_out elf_load_process(fat32_mount_t *mount, const char *filename) {

    elf_load_out result = {0};

    if (mount == NULL || filename == NULL) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    // Pin the file for demand paging. Every segment region takes its own
    // reference on this backing, so the loader's reference is dropped
    // unconditionally at the end - the mappings keep the file alive.
    fs_mmap_pin_out pin_md = fs_mmap_pin_path(mount, filename);
    if (!pin_md.success || pin_md.backing == NULL) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    elf_eheader_t header = {0};
    if (!elf_read_header(pin_md.backing, &header) || header.e_entry == 0) {
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    if (header.e_phnum == 0 || header.e_phentsize < sizeof(elf_pheader_t)) {
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    uint64_t phdr_bytes = (uint64_t)header.e_phnum * header.e_phentsize;

    // The program header table is the only part of the file that still has to
    // fit in a heap allocation, and kmalloc tops out at one page. That is 64
    // entries at the standard 56-byte size, far more than any binary this
    // kernel loads, but reject it explicitly rather than truncating.
    if (phdr_bytes > 4096) {
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    if (header.e_phoff > pin_md.file_size ||
        phdr_bytes > (uint64_t)pin_md.file_size - header.e_phoff) {
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    uint8_t *phdr_table = (uint8_t*)kmalloc((size_t)phdr_bytes);
    if (phdr_table == NULL) {
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    if (fs_mmap_read(pin_md.backing, header.e_phoff, phdr_table, phdr_bytes) < 0) {
        kfree(phdr_table);
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    create_proc_out proc_md = create_proc(false);
    if (!proc_md.success) {
        kfree(phdr_table);
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_PROC_CREATE_FAILED;
        return result;
    }

    elf_map_out map_md = elf_map_segments(proc_md.proc->adress_space, phdr_table,
                                          &header, pin_md.backing,
                                          (uint64_t)pin_md.file_size);

    kfree(phdr_table);

    if (!map_md.success) {
        elf_abandon_proc(proc_md.proc);
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_MAP_FAILED;
        return result;
    }

    create_thread_out thread_md = create_thread(proc_md.proc, header.e_entry, 0, 64 * 1024);
    if (!thread_md.success) {
        // create_thread() cleans up after itself on every failure path and
        // only links the thread into the process once it has fully succeeded,
        // so there is nothing attached here to block the teardown.
        elf_abandon_proc(proc_md.proc);
        (void)fs_mmap_unpin(pin_md.backing);
        result.error_code = ELF_LOAD_THREAD_CREATE_FAILED;
        return result;
    }

    // Loader's own reference. The segment regions hold theirs until the
    // address space is torn down.
    (void)fs_mmap_unpin(pin_md.backing);

    result.success = true;
    result.p = proc_md.proc;
    result.t = thread_md.thread;
    return result;
}
