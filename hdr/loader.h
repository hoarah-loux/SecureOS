#include <elfinfo.h>
#include <fat32_fs.h>
#include <heap.h>
#include <t.h>

typedef struct {
    uint8_t* data;
    uint32_t size;
    const char* filename;
} mem_file_t;

mem_file_t* load_file(fat32_mount_t* mount, char* filename);
elf_eheader_t parse_mem_elf(mem_file_t elf);

typedef struct elf_map_out {
    bool success;
    uint64_t error_code;
} elf_map_out;

enum elf_map_error {
    ELF_MAP_INVALID_ARGS,
    ELF_MAP_PHDR_OUT_OF_BOUNDS,
    ELF_MAP_RESERVE_FAILED,
    ELF_MAP_FRAME_ALLOC_FAILED,
    ELF_MAP_PAGE_MAP_FAILED,
    ELF_MAP_COPY_VERIFY_FAILED,
};

// walks header->e_phnum program headers in file->data and, for every
// PT_LOAD segment, reserves+maps+populates it in space. eager (not
// demand-paged): every page is allocated, zeroed, and mapped up front.
elf_map_out elf_map_segments(thread *t, mem_file_t *file, elf_eheader_t *header);

typedef struct elf_load_out {
    bool success;
    uint64_t error_code;
    proc *p;
    thread *t;
} elf_load_out;

enum elf_load_error {
    ELF_LOAD_FILE_READ_FAILED,
    ELF_LOAD_INVALID_ELF,
    ELF_LOAD_PROC_CREATE_FAILED,
    ELF_LOAD_MAP_FAILED,
    ELF_LOAD_THREAD_CREATE_FAILED,
    ELF_LOAD_SCHEDULER_FAILED
};

// full pipeline: load filename off mount, parse+validate its ELF header,
// create a process, map its PT_LOAD segments, and create its main thread
// ready to run at e_entry in ring3.
elf_load_out elf_load_process(fat32_mount_t *mount, const char *filename);