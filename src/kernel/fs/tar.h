#include <stdint.h>
#include <stddef.h>

// This struct is used to read the initrd tar file header.
// initrd accepts any format but for our case we're using a simple tar file
typedef struct {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];     // Octal size string
    char mtime[12];
    char chksum[8];
    char typeflag;     // '0' = normal file, '5' = directory
    char linkname[100];
} tar_header_t;