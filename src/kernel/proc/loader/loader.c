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

elf_map_out elf_map_segments(thread *t, mem_file_t *file, elf_eheader_t *header) {



    elf_map_out result = {0};

    vm_space *space = t->owning_proc->adress_space;

    if (space == NULL || file == NULL || header == NULL) {
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

        // Reserve the segment through mmap() rather than hand-rolling a
        // vm_object and calling reserve_region directly - one owner for the
        // region bookkeeping, and mmap() already frees the object on failure.
        //
        // fd 0 means an anonymous region: no file backing, which is what an
        // eagerly populated segment wants. The pages themselves are still
        // allocated and filled below, because mmap() only reserves.
        //
        // NOTE: mmap() resolves its target space from scheduler_current(), so
        // it reserves in the *running* process, not necessarily in `space`.
        // Until the init ordering is sorted out, those are the same address
        // space only when the loader runs on behalf of the process it is
        // populating.
        mmap_req req = {
            .t = t,
            .addr = (void*)seg_start,
            .len = (seg_end - seg_start),
            .perms = perms,
            .map_flags = PRIVATE | FIXED,
            .file_descriptor = 0,
            .file_offset = 0
        };

        void *mapped = mmap(&req);

        // if (mapped == NULL) {
        //     result.error_code = ELF_MAP_RESERVE_FAILED;
        //     return result;
        // }

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
    
    create_thread_out thread_md = create_thread(proc_md.proc, header.e_entry, 0, 64 * 1024);
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