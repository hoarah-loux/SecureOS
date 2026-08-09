#include <loader.h>

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