#include <stdint.h>

#define EI_NIDENT 16

typedef struct {
    unsigned char	e_ident[EI_NIDENT];	/* Magic number and other info */
    uint16_t	e_type;			/* Object file type */
    uint16_t	e_machine;		/* Architecture */
    uint32_t	e_version;		/* Object file version */
    uint64_t	e_entry;		/* Entry point virtual address */
    uint64_t	e_phoff;		/* Program header table file offset */
    uint64_t	e_shoff;		/* Section header table file offset */
    uint32_t	e_flags;		/* Processor-specific flags */
    uint16_t	e_ehsize;		/* ELF header size in bytes */
    uint16_t	e_phentsize;		/* Program header table entry size */
    uint16_t	e_phnum;		/* Program header table entry count */
    uint16_t	e_shentsize;		/* Section header table entry size */
    uint16_t	e_shnum;		/* Section header table entry count */
    uint16_t	e_shstrndx;		/* Section header string table index */
} elf_eheader_t;

/* e_ident[EI_CLASS] value for a 64-bit object, and the e_type of a
 * non-relocatable executable. The loader accepts only these two: it maps
 * segments at their p_vaddr as written, with no relocation step. */
#define ELFCLASS64 2
#define EI_CLASS   4
#define ET_EXEC    2

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    uint32_t	p_type;			/* Segment type */
    uint32_t	p_flags;		/* Segment flags */
    uint64_t	p_offset;		/* Segment file offset */
    uint64_t	p_vaddr;		/* Segment virtual address */
    uint64_t	p_paddr;		/* Segment physical address */
    uint64_t	p_filesz;		/* Segment size in file */
    uint64_t	p_memsz;		/* Segment size in memory */
    uint64_t	p_align;		/* Segment alignment */
} elf_pheader_t;

typedef struct {
    uint32_t	sh_name;		/* Section name (string tbl index) */
    uint32_t	sh_type;		/* Section type */
    uint64_t	sh_flags;		/* Section flags */
    uint64_t	sh_addr;		/* Section virtual addr at execution */
    uint64_t	sh_offset;		/* Section file offset */
    uint64_t	sh_size;		/* Section size in bytes */
    uint32_t	sh_link;		/* Link to another section */
    uint32_t	sh_info;		/* Additional section information */
    uint64_t	sh_addralign;		/* Section alignment */
    uint64_t	sh_entsize;		/* Entry size if section holds table */
} elf_sheader_t;