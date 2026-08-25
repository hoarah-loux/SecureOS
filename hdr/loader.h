#include <elfinfo.h>
#include <fat32_fs.h>
#include <heap.h>
#include <t.h>

// An executable the loader is working on, minus its contents.
//
// The loader never holds an executable's bytes: text and data reach the
// process through mmap()'d file-backed regions that the #PF handler fills one
// page at a time. All the kernel keeps is an open handle plus the parsed ELF
// header, so the loader's own memory use is the same for a 1 KiB binary and a
// 100 MiB one.
//
// file stays open for as long as the image is in use - program headers are
// read off it individually during mapping - and is released by
// elf_close_image().
typedef struct elf_image {
    fat32_file_t file;
    elf_eheader_t header;
    // Size from the directory entry, and the bound every p_offset / p_filesz
    // in the file is validated against.
    uint32_t file_size;
} elf_image_t;

typedef struct elf_open_out {
    bool success;
    // One of enum elf_load_error, so a caller can pass it straight out.
    uint64_t error_code;
} elf_open_out;

// Opens filename on mount and reads and validates its ELF header into
// out_image. On success out_image->file is open and belongs to the caller.
elf_open_out elf_open_image(fat32_mount_t *mount, const char *filename, elf_image_t *out_image);

// Closes the handle an elf_image_t holds. Safe on a zeroed or already closed
// image.
void elf_close_image(elf_image_t *image);

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
    ELF_MAP_PHDR_READ_FAILED,
};

// Reads image's e_phnum program headers off disk one at a time and, for every
// PT_LOAD segment, mmap()s a file-backed region for it in t's address space.
// Demand-paged: nothing is read or allocated here beyond the program headers
// themselves - the #PF handler populates each page from the file on first
// touch, and .bss tails arrive zero-filled.
//
// Requires t's proc to already hold the executable open at fd PROC_FD_BASE:
// that is the descriptor the regions are backed by.
elf_map_out elf_map_segments(thread *t, elf_image_t *image);

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