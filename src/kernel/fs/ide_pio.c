#include "fs/ide_pio.h"

#include "core/serial.h"

enum {
    IDE_PRIMARY_IO_BASE = 0x1F0,
    IDE_PRIMARY_CONTROL_BASE = 0x3F6,
    IDE_SECONDARY_IO_BASE = 0x170,
    IDE_SECONDARY_CONTROL_BASE = 0x376,
    IDE_SECTOR_SIZE = 512,
    IDE_STATUS_ERR = 0x01,
    IDE_STATUS_DRQ = 0x08,
    IDE_STATUS_DF = 0x20,
    IDE_STATUS_BSY = 0x80,
    IDE_CMD_READ_SECTORS = 0x20,
    IDE_CMD_WRITE_SECTORS = 0x30,
    IDE_CMD_CACHE_FLUSH = 0xE7,
    IDE_CMD_IDENTIFY = 0xEC,
};

typedef struct {
    uint16_t io_base;
    uint16_t control_base;
    uint8_t device_select;
    uint64_t sector_count;
} ide_pio_backend_t;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void insw(uint16_t port, void *buffer, uint32_t word_count) {
    __asm__ volatile("rep insw" : "+D"(buffer), "+c"(word_count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buffer, uint32_t word_count) {
    const void *src = buffer;
    __asm__ volatile("rep outsw" : "+S"(src), "+c"(word_count) : "d"(port) : "memory");
}

static void ide_io_wait(uint16_t control_base) {
    (void)inb((uint16_t)(control_base + 0));
    (void)inb((uint16_t)(control_base + 0));
    (void)inb((uint16_t)(control_base + 0));
    (void)inb((uint16_t)(control_base + 0));
}

static void ide_log_probe(uint16_t io_base, uint8_t device_select, const char *result) {
    serial_write_string("[IDE] probe io=0x");
    serial_write_hex32(io_base);
    serial_write_string(" dev=0x");
    serial_write_hex32(device_select);
    serial_write_string(" ");
    serial_write_string(result);
    serial_write_string("\r\n");
}

static int ide_wait_for_drq(ide_pio_backend_t *backend) {
    for (uint32_t attempt = 0; attempt < 1000000u; attempt++) {
        uint8_t status = inb((uint16_t)(backend->io_base + 7));
        if ((status & IDE_STATUS_BSY) != 0) {
            continue;
        }
        if ((status & (IDE_STATUS_ERR | IDE_STATUS_DF)) != 0) {
            return -1;
        }
        if ((status & IDE_STATUS_DRQ) != 0) {
            return 0;
        }
    }

    return -1;
}

static int ide_wait_for_complete(ide_pio_backend_t *backend) {
    for (uint32_t attempt = 0; attempt < 1000000u; attempt++) {
        uint8_t status = inb((uint16_t)(backend->io_base + 7));
        if ((status & IDE_STATUS_BSY) != 0) {
            continue;
        }
        if ((status & (IDE_STATUS_ERR | IDE_STATUS_DF)) != 0) {
            return -1;
        }
        return 0;
    }

    return -1;
}

static int ide_identify(ide_pio_backend_t *backend) {
    outb((uint16_t)(backend->io_base + 6), backend->device_select);
    ide_io_wait(backend->control_base);

    outb((uint16_t)(backend->io_base + 2), 0);
    outb((uint16_t)(backend->io_base + 3), 0);
    outb((uint16_t)(backend->io_base + 4), 0);
    outb((uint16_t)(backend->io_base + 5), 0);
    outb((uint16_t)(backend->io_base + 7), IDE_CMD_IDENTIFY);

    uint8_t status = inb((uint16_t)(backend->io_base + 7));
    if (status == 0) {
        return -1;
    }

    if (ide_wait_for_drq(backend) != 0) {
        return -1;
    }

    uint16_t identify_data[256];
    insw(backend->io_base, identify_data, 256u);
    ide_io_wait(backend->control_base);

    uint64_t sector_count = (uint64_t)identify_data[60] | ((uint64_t)identify_data[61] << 16);
    if (sector_count == 0) {
        sector_count = ((uint64_t)identify_data[100])
            | ((uint64_t)identify_data[101] << 16)
            | ((uint64_t)identify_data[102] << 32)
            | ((uint64_t)identify_data[103] << 48);
    }

    if (sector_count == 0) {
        return -1;
    }

    backend->sector_count = sector_count;
    return 0;
}

static int ide_pio_read(void *context, uint64_t sector, uint32_t count, uint8_t *buffer) {
    ide_pio_backend_t *backend = (ide_pio_backend_t *)context;

    if (!backend || !buffer) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if (backend->sector_count != 0) {
        if (sector >= backend->sector_count || count > backend->sector_count - sector) {
            return -1;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t lba = sector + i;

        outb((uint16_t)(backend->io_base + 6), (uint8_t)(backend->device_select | (uint8_t)((lba >> 24) & 0x0Fu)));
        outb((uint16_t)(backend->io_base + 2), 1);
        outb((uint16_t)(backend->io_base + 3), (uint8_t)(lba & 0xFFu));
        outb((uint16_t)(backend->io_base + 4), (uint8_t)((lba >> 8) & 0xFFu));
        outb((uint16_t)(backend->io_base + 5), (uint8_t)((lba >> 16) & 0xFFu));
        outb((uint16_t)(backend->io_base + 7), IDE_CMD_READ_SECTORS);

        if (ide_wait_for_drq(backend) != 0) {
            return -1;
        }

        insw(backend->io_base, buffer + ((uint64_t)i * IDE_SECTOR_SIZE), IDE_SECTOR_SIZE / 2u);
        ide_io_wait(backend->control_base);
    }

    return 0;
}

static int ide_pio_write(void *context, uint64_t sector, uint32_t count, const uint8_t *buffer) {
    ide_pio_backend_t *backend = (ide_pio_backend_t *)context;

    if (!backend || !buffer) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if (backend->sector_count != 0) {
        if (sector >= backend->sector_count || count > backend->sector_count - sector) {
            return -1;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t lba = sector + i;

        outb((uint16_t)(backend->io_base + 6), (uint8_t)(backend->device_select | (uint8_t)((lba >> 24) & 0x0Fu)));
        outb((uint16_t)(backend->io_base + 2), 1);
        outb((uint16_t)(backend->io_base + 3), (uint8_t)(lba & 0xFFu));
        outb((uint16_t)(backend->io_base + 4), (uint8_t)((lba >> 8) & 0xFFu));
        outb((uint16_t)(backend->io_base + 5), (uint8_t)((lba >> 16) & 0xFFu));
        outb((uint16_t)(backend->io_base + 7), IDE_CMD_WRITE_SECTORS);

        if (ide_wait_for_drq(backend) != 0) {
            return -1;
        }

        outsw(backend->io_base, buffer + ((uint64_t)i * IDE_SECTOR_SIZE), IDE_SECTOR_SIZE / 2u);
        ide_io_wait(backend->control_base);

        if (ide_wait_for_complete(backend) != 0) {
            return -1;
        }
    }

    outb((uint16_t)(backend->io_base + 7), IDE_CMD_CACHE_FLUSH);
    if (ide_wait_for_complete(backend) != 0) {
        return -1;
    }

    return 0;
}

static const block_device_ops_t ide_pio_device_ops = {
    .read = ide_pio_read,
    .write = ide_pio_write,
};

static int ide_try_probe(block_device_t *device, const char *name, uint16_t io_base, uint16_t control_base, uint8_t device_select) {
    ide_pio_backend_t *backend = (ide_pio_backend_t *)(void *)device->backend_storage;

    backend->io_base = io_base;
    backend->control_base = control_base;
    backend->device_select = device_select;
    backend->sector_count = 0;

    if (ide_identify(backend) != 0) {
        ide_log_probe(io_base, device_select, "failed");
        return -1;
    }

    ide_log_probe(io_base, device_select, "ok");

    device->name = name;
    device->sector_size = IDE_SECTOR_SIZE;
    device->sector_count = backend->sector_count;
    device->context = backend;
    device->ops = &ide_pio_device_ops;

    return 0;
}

int block_device_init_ide_primary_master(block_device_t *device, const char *name) {
    if (!device) {
        return -1;
    }

    if (ide_try_probe(device, name, IDE_PRIMARY_IO_BASE, IDE_PRIMARY_CONTROL_BASE, 0xE0u) == 0) {
        return 0;
    }

    if (ide_try_probe(device, name, IDE_PRIMARY_IO_BASE, IDE_PRIMARY_CONTROL_BASE, 0xF0u) == 0) {
        return 0;
    }

    if (ide_try_probe(device, name, IDE_SECONDARY_IO_BASE, IDE_SECONDARY_CONTROL_BASE, 0xE0u) == 0) {
        return 0;
    }

    if (ide_try_probe(device, name, IDE_SECONDARY_IO_BASE, IDE_SECONDARY_CONTROL_BASE, 0xF0u) == 0) {
        return 0;
    }

    return -1;
}
