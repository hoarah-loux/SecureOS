#include <elfinfo.h>
#include <fat32_fs.h>
#include <fs_syscalls.h>
#include <heap.h>
#include <t.h>

typedef struct elf_map_out {
    bool success;
    uint64_t error_code;
} elf_map_out;

enum elf_map_error {
    ELF_MAP_INVALID_ARGS,
    ELF_MAP_PHDR_OUT_OF_BOUNDS,
    ELF_MAP_BAD_FILE_ALIGNMENT,
    ELF_MAP_MMAP_FAILED,
};

// walks header->e_phnum program headers in file->data and, for every
// PT_LOAD segment, mmap()s it into space. demand paged: nothing is allocated
// or mapped here, only reserved - the page fault handler allocates a frame
// and fills it (from `backing` for the file part of a segment, with zeroes
// for the .bss part) the first time each page is touched.
//
// Each segment becomes up to two regions: a PRIVATE file-backed one covering
// [p_vaddr, p_vaddr + p_filesz), and an anonymous one for whatever .bss
// remains past that region's final page. Slack in that final page reads as
// zero, so a .bss tail sharing a page with file data is handled correctly.
//
// `phdr_table` is the program header table only (e_phnum * e_phentsize bytes
// read from e_phoff), not the whole file - nothing past the headers has to be
// in memory for a demand-paged load. `backing` must be a pinned backing for
// the file those headers came from, and `file_size` its size on disk, used to
// bounds check every segment. The caller keeps ownership of its reference on
// `backing`; each region takes its own.
elf_map_out elf_map_segments(vm_space *space, const uint8_t *phdr_table,
                             const elf_eheader_t *header, fs_mmap_backing *backing,
                             uint64_t file_size);

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
};

// full pipeline: load filename off mount, parse+validate its ELF header,
// create a process, map its PT_LOAD segments, and create its main thread
// ready to run at e_entry in ring3.
elf_load_out elf_load_process(fat32_mount_t *mount, const char *filename);