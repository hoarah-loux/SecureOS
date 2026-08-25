/*
 * CAT.EXE - milestone 3: read a multi-megabyte file off FAT32 through newlib
 * stdio, from ring3, and prove the bytes are the right ones.
 *
 * The file is /DOOM1.WAD, 4196020 bytes, because that is the file doom has to
 * read and this is the test that says whether it can. Nothing here is doom
 * specific beyond the constants: the point is stdio buffering, seeking,
 * sequential throughput and the FAT32 cluster chain walk under a real load.
 *
 * The expected checksums are computed on the host from the same file
 * (sha256 1d7d43be...), so a wrong byte anywhere - a misread cluster, an
 * off-by-one in the chain walk, a stale FAT sector cache - shows up as a
 * mismatch rather than as doom crashing later for no visible reason.
 *
 * There is no argv, so the path is fixed. See src/userland/README.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WAD_PATH        "/DOOM1.WAD"
#define WAD_SIZE        4196020u
#define WAD_NUMLUMPS    1264u
#define WAD_DIR_OFFSET  4175796u
#define WAD_SUM         0x1683486du   /* 32-bit sum of every byte */
#define WAD_FNV         0x03c09342u   /* FNV-1a over every byte */

#define CHUNK 8192

static uint32_t now_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) { return 0; }
    return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

int main(void)
{
    static uint8_t chunk[CHUNK];

    printf("[CAT] step 1: opening %s\n", WAD_PATH);

    FILE *f = fopen(WAD_PATH, "rb");
    if (f == NULL) {
        printf("[CAT] FAIL: cannot open %s\n", WAD_PATH);
        return 1;
    }

    /* stdio sizes its reads from st_blksize and needs st_size to know where
     * the file ends; both come from _fstat, which fakes them with lseek. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        printf("[CAT] FAIL: fstat failed\n");
        return 1;
    }

    if ((uint32_t)st.st_size != WAD_SIZE) {
        printf("[CAT] FAIL: size %lu, expected %u\n",
               (unsigned long)st.st_size, WAD_SIZE);
        return 1;
    }
    printf("[CAT] step 2: size %lu bytes\n", (unsigned long)st.st_size);

    /* The 12-byte WAD header: magic, lump count, directory offset. */
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        printf("[CAT] FAIL: short header read\n");
        return 1;
    }

    if (memcmp(header, "IWAD", 4) != 0) {
        printf("[CAT] FAIL: magic %02x %02x %02x %02x, expected IWAD\n",
               header[0], header[1], header[2], header[3]);
        return 1;
    }

    uint32_t numlumps = (uint32_t)header[4] | ((uint32_t)header[5] << 8)
                      | ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
    uint32_t diroffset = (uint32_t)header[8] | ((uint32_t)header[9] << 8)
                       | ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);

    if (numlumps != WAD_NUMLUMPS || diroffset != WAD_DIR_OFFSET) {
        printf("[CAT] FAIL: header says %u lumps at %u, expected %u at %u\n",
               numlumps, diroffset, WAD_NUMLUMPS, WAD_DIR_OFFSET);
        return 1;
    }
    printf("[CAT] step 3: header ok, %u lumps, directory at %u\n",
           numlumps, diroffset);

    /* Seek to the far end of the file and read the first directory entry.
     * This is the operation doom does thousands of times and the one the
     * cluster chain walk is worst at, so it is worth testing on its own. */
    if (fseek(f, (long)diroffset, SEEK_SET) != 0) {
        printf("[CAT] FAIL: seek to directory failed\n");
        return 1;
    }

    uint8_t entry[16];
    if (fread(entry, 1, sizeof(entry), f) != sizeof(entry)) {
        printf("[CAT] FAIL: short directory read\n");
        return 1;
    }

    if (memcmp(entry + 8, "PLAYPAL", 7) != 0) {
        printf("[CAT] FAIL: first lump is not PLAYPAL\n");
        return 1;
    }
    printf("[CAT] step 4: first lump PLAYPAL, %u bytes at %u\n",
           (unsigned)((uint32_t)entry[4] | ((uint32_t)entry[5] << 8)
                    | ((uint32_t)entry[6] << 16) | ((uint32_t)entry[7] << 24)),
           (unsigned)((uint32_t)entry[0] | ((uint32_t)entry[1] << 8)
                    | ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24)));

    /* The whole file, sequentially, checksummed. */
    if (fseek(f, 0, SEEK_SET) != 0) {
        printf("[CAT] FAIL: rewind failed\n");
        return 1;
    }

    uint32_t sum = 0;
    uint32_t fnv = 2166136261u;
    uint32_t total = 0;
    uint32_t start = now_ms();

    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), f);
        if (got == 0) { break; }

        for (size_t i = 0; i < got; i++) {
            sum += chunk[i];
            fnv = (fnv ^ chunk[i]) * 16777619u;
        }

        total += (uint32_t)got;
    }

    uint32_t elapsed = now_ms() - start;

    if (total != WAD_SIZE) {
        printf("[CAT] FAIL: read %u bytes, expected %u\n", total, WAD_SIZE);
        return 1;
    }

    if (sum != WAD_SUM || fnv != WAD_FNV) {
        printf("[CAT] FAIL: checksum sum=0x%08x fnv=0x%08x, expected 0x%08x/0x%08x\n",
               sum, fnv, WAD_SUM, WAD_FNV);
        return 1;
    }

    /* Guard against a zero elapsed time: the clock has 10 ms resolution, and
     * dividing by it would trap. */
    if (elapsed == 0) { elapsed = 1; }

    printf("[CAT] step 5: %u bytes read, checksums match, %u ms, %u KB/s\n",
           total, elapsed, (total / 1024u) * 1000u / elapsed);

    fclose(f);

    /* fd 3 is the executable this program is running from, demand-paged for
     * the life of the process. The kernel must refuse to close it. */
    if (close(3) == 0) {
        printf("[CAT] FAIL: kernel allowed close(3) on the executable\n");
        return 1;
    }
    printf("[CAT] step 6: close(3) on the executable refused\n");

    printf("[CAT] done, exiting 0\n");
    return 0;
}
