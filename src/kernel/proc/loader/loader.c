#include <loader.h>
#include <pmm.h>
#include <string.h>
#include <scheduler.h>

// This function takes a filename and loaded it into memory, return a pointer
mem_file_t* load_file(fat32_mount_t* mount, char* filename) {

    fat32_file_t file = { 0 };
    int open_result = fat32_file_open(mount, filename, &file);

    if (open_result)
        return (mem_file_t*)NULL;

    mem_file_t* mem_file = (mem_file_t*)kmalloc(sizeof(mem_file_t));

    if (!mem_file) {
        fat32_file_close(&file);
        return (mem_file_t*)NULL;
    }

    mem_file->data = (uint8_t*)kmalloc(file.entry.file_size);
    if (!mem_file->data) {
        kfree(mem_file);
        fat32_file_close(&file);
        return (mem_file_t*)NULL;
    }

    mem_file->size = file.entry.file_size;
    mem_file->filename = filename;

    int read_result = fat32_file_read(&file, mem_file->data, file.entry.file_size);
    if (read_result != file.entry.file_size) {
        kfree(mem_file->data);
        kfree(mem_file);
        fat32_file_close(&file);
        return (mem_file_t*)NULL;
    }

    fat32_file_close(&file);
    return mem_file;
}

elf_eheader_t parse_mem_elf(mem_file_t elf) {
    // check elf size first
    if (elf.size < sizeof(elf_eheader_t)) {
        // Not a valid ELF file
        elf_eheader_t invalid_header = {0};
        return invalid_header;
    }

    // Read first 4 bytes to check for ELF magic number
    if (elf.data[0] != 0x7F || elf.data[1] != 'E' || elf.data[2] != 'L' || elf.data[3] != 'F') {
        // Not a valid ELF file
        elf_eheader_t invalid_header = {0};
        return invalid_header;
    }

    // Cast the data to an ELF header
    elf_eheader_t header = *(elf_eheader_t*)elf.data;
    return header;
}

// Frees a mem_file_t and its payload. load_file() allocates the two
// separately, so both have to go back.
static void free_mem_file(mem_file_t *file) {

    if (file == NULL) { return; }

    if (file->data != NULL) { kfree(file->data); }

    kfree(file);
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
        if (p->fd_table[fd].in_use && p->fd_table[fd].kind == PROC_FD_FILE) {
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

elf_map_out elf_map_segments(thread *t, mem_file_t *file, elf_eheader_t *header) {



    elf_map_out result = {0};

    if (t == NULL || t->owning_proc == NULL || file == NULL || header == NULL) {
        result.error_code = ELF_MAP_INVALID_ARGS;
        return result;
    }

    vm_space *space = t->owning_proc->adress_space;

    if (space == NULL) {
        result.error_code = ELF_MAP_INVALID_ARGS;
        return result;
    }

    if (header->e_phentsize < sizeof(elf_pheader_t)) {
        result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
        return result;
    }

    uint64_t phdr_table_bytes = (uint64_t)header->e_phnum * header->e_phentsize;
    if (header->e_phoff > file->size || phdr_table_bytes > (uint64_t)file->size - header->e_phoff) {
        result.error_code = ELF_MAP_PHDR_OUT_OF_BOUNDS;
        return result;
    }

    for (uint16_t i = 0; i < header->e_phnum; i++) {

        elf_pheader_t *phdr = (elf_pheader_t*)(file->data + header->e_phoff + (uint64_t)i * header->e_phentsize);

        if (phdr->p_type != PT_LOAD) { continue; }

        if (phdr->p_offset > file->size || phdr->p_filesz > (uint64_t)file->size - phdr->p_offset) {
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
        mmap_req req = {
            .t = t,
            .addr = (void*)seg_start,
            .len = (seg_end - seg_start),
            .perms = perms,
            .map_flags = PRIVATE | FIXED,
            .file_descriptor = PROC_FD_BASE,
            .file_offset = seg_file_offset
        };

        void *mapped = mmap(&req);

        if (mapped == NULL) {
            result.error_code = ELF_MAP_RESERVE_FAILED;
            return result;
        }

        // mmap() hands back the VA, not the region. Look the region back up so
        // the ELF_SEGMENT tag and the teardown path below still work.
        search_region_list_out seg_search = search_region_list(space, (uintptr_t)mapped);
        vm_region *seg_region = seg_search.success ? seg_search.region : NULL;
        if (seg_region != NULL) { seg_region->type = ELF_SEGMENT; }

     
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

    mem_file_t *file = load_file(mount, (char*)filename);
    if (file == NULL) {
        result.error_code = ELF_LOAD_FILE_READ_FAILED;
        return result;
    }

    elf_eheader_t header = parse_mem_elf(*file);
    if (header.e_entry == 0) {
        result.error_code = ELF_LOAD_INVALID_ELF;
        return result;
    }

    create_proc_out proc_md = create_proc(false);
    if (!proc_md.success) {
        result.error_code = ELF_LOAD_PROC_CREATE_FAILED;
        return result;
    }
    
    int slot = PROC_FD_BASE;
    fat32_file_open(mount, filename, &proc_md.proc->fd_table[slot].file);
    proc_md.proc->fd_table[slot].in_use = 1;
    proc_md.proc->fd_table[slot].kind   = PROC_FD_FILE;

    create_thread_out thread_md = create_thread(proc_md.proc, header.e_entry, THREAD_F_MAIN_THREAD, 64 * 1024);
    if (!thread_md.success) {
        result.error_code = ELF_LOAD_THREAD_CREATE_FAILED;
        return result;
    }
    
    elf_map_out map_md = elf_map_segments(thread_md.thread, file, &header);
    if (!map_md.success) {
        result.error_code = ELF_LOAD_MAP_FAILED;
        return result;
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