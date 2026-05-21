#ifndef TAR_H
#define TAR_H
#include <stdint.h>
#include <stddef.h>

#define OLDGNU_MAGIC "ustar  "  /* 7 chars and a null */

#define REGTYPE  '0'            /* regular file */
#define AREGTYPE '\0'           /* regular file */
#define LNKTYPE  '1'            /* link */
#define SYMTYPE  '2'            /* reserved */
#define CHRTYPE  '3'            /* character special */
#define BLKTYPE  '4'            /* block special */
#define DIRTYPE  '5'            /* directory */
#define FIFOTYPE '6'            /* FIFO special */
#define CONTTYPE '7'            /* reserved */

// This struct is used to read the initrd tar file header.
// initrd accepts any format but for our case we're using a simple tar file
typedef struct {
    char filename[100];      /*   0-99 */
    char mode[8];        /* 100-107 */
    char uid[8];         /* 108-115 */
    char gid[8];         /* 116-123 */
    char size[12];       /* 124-135 */
    char mtime[12];      /* 136-147 */
    char chksum[8];      /* 148-155 */
    char typeflag;       /* 156 */
                        /* 157-512 is unused padding */
} tar_header_t;

uint32_t o2d(const char* octal);
uint32_t parse_headers(uintptr_t tar_start);
int tar_print_file(uintptr_t tar_start, const char *name);


#endif