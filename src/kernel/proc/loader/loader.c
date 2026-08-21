#include <loader.h>
#include <pmm.h>
#include <string.h>

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

elf_map_out elf_map_segments(vm_space *space, mem_file_t *file, elf_eheader_t *header) {

    elf_map_out result = {0};

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

        reserve_region_request req = {
            .size = seg_end - seg_start,
            .type = ELF_SEGMENT,
            .object = NULL,
            .perms = perms,
            .map_flags = FIXED,
            .hint_va = seg_start,
            .backing_offset = 0,
        };

        reserve_region_out reserve_md = reserve_region(space, &req);
        if (!reserve_md.success) {
            result.error_code = ELF_MAP_RESERVE_FAILED;
            return result;
        }

        for (uintptr_t page_va = seg_start; page_va < seg_end; page_va += 4096) {

            uintptr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                result.error_code = ELF_MAP_FRAME_ALLOC_FAILED;
                return result;
            }

            void *page_kva = (void*)phys_to_virt(frame);
            memset(page_kva, 0, 4096);

            // copy whatever part of this page falls within
            // [p_vaddr, p_vaddr + p_filesz) from the file; anything beyond
            // that (the BSS tail, up to p_memsz) stays zeroed from above.
            uintptr_t page_va_end = page_va + 4096;
            uintptr_t seg_file_start = phdr->p_vaddr;
            uintptr_t seg_file_end = phdr->p_vaddr + phdr->p_filesz;
            uintptr_t copy_start = (page_va > seg_file_start) ? page_va : seg_file_start;
            uintptr_t copy_end = (page_va_end < seg_file_end) ? page_va_end : seg_file_end;

            if (copy_start < copy_end) {
                uint64_t copy_len = copy_end - copy_start;
                uint64_t page_offset = copy_start - page_va;
                uint64_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);
                memcpy((uint8_t*)page_kva + page_offset, file->data + file_offset, copy_len);

                // self-check: a silently wrong copy here is a silently
                // broken process later, so verify it before moving on.
                if (memcmp((uint8_t*)page_kva + page_offset, file->data + file_offset, copy_len) != 0) {
                    result.error_code = ELF_MAP_COPY_VERIFY_FAILED;
                    return result;
                }
            }

            // vmm_map_page/the_walking_pager2 expect a PHYSICAL root table
            // pointer (they phys_to_virt() it internally, same convention
            // handlers.c's lazy-alloc path uses) - pml4_phys_ptr, not the
            // already-virtual pml4_ptr.
            map_out map_md = vmm_map_page(page_va, frame, space->pml4_phys_ptr, 4, perms);
            if (!map_md.success) {
                result.error_code = ELF_MAP_PAGE_MAP_FAILED;
                return result;
            }
        }
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

    elf_map_out map_md = elf_map_segments(proc_md.proc->adress_space, file, &header);
    if (!map_md.success) {
        result.error_code = ELF_LOAD_MAP_FAILED;
        return result;
    }

    create_thread_out thread_md = create_thread(proc_md.proc, header.e_entry, 0, 64 * 1024);
    if (!thread_md.success) {
        result.error_code = ELF_LOAD_THREAD_CREATE_FAILED;
        return result;
    }

    result.success = true;
    result.p = proc_md.proc;
    result.t = thread_md.thread;
    return result;
}