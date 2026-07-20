#include <loader.h>

// This function takes a filename and loaded it into memory, return a pointer
mem_file_t* load_file(fat32_mount_t* mount, char* filename) {

    fat32_file_t file = { 0 };
    int open_result = fat32_file_open(mount, filename, &file);

    if (!open_result)
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