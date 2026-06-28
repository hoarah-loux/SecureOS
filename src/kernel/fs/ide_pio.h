#ifndef IDE_PIO_H
#define IDE_PIO_H

#include <stdint.h>

#include "fs/block_device.h"

int block_device_init_ide_primary_master(block_device_t *device, const char *name);

#endif /* IDE_PIO_H */