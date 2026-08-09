#include <elfinfo.h>
#include <fat32_fs.h>
#include <heap.h>

typedef struct {
    uint8_t* data;
    uint32_t size;
    const char* filename;
} mem_file_t;

mem_file_t* load_file(fat32_mount_t* mount, char* filename);
elf_eheader_t parse_mem_elf(mem_file_t elf);