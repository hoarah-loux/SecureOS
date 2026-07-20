#include <elf.h>
#include <fat32_fs.h>
#include <heap.h>

typedef struct {
    uint8_t* data;
    uint32_t size;
    const char* filename;
} mem_file_t;

mem_file_t* load_file(const char* filename);