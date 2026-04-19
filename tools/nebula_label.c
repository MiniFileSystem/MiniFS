/*
 * nebula_label.c - Read / write the Nebula device label (MBR).
 *
 * Usage:
 *   nebula_label read  --path <file>
 *   nebula_label write --path <file> [--uuid <uuid-or-"auto">]
 *                      [--capacity-blocks <N>]
 *
 * `read` prints the current MBR in human-readable form.
 * `write` stamps a new MBR. If the device already has one, we preserve
 * the capacity unless --capacity-blocks is given.
 */
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"

#include "../src/util/uuid.h"
#include "../src/util/log.h"
#include "../src/nebula/nebula_mbr.h"
#include "../src/nebula/nebula_superblock.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s read  --path <file>\n"
        "  %s write --path <file> [--uuid <hex|auto>] [--capacity-blocks N]\n",
        p, p);
}

/* Parse "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" (32 hex, optional dashes) into 16B. */
static int parse_uuid(const char *s, uint8_t out[16])
{
    uint8_t tmp[16];
    size_t n = 0;
    const char *p = s;
    while (*p && n < 16) {
        if (*p == '-') { p++; continue; }
        int hi = -1, lo = -1;
        char c = *p++;
        if (isdigit((unsigned char)c))      hi = c - '0';
        else if (c >= 'a' && c <= 'f')      hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')      hi = c - 'A' + 10;
        else return -EINVAL;
        c = *p++;
        if (!c) return -EINVAL;
        if (isdigit((unsigned char)c))      lo = c - '0';
        else if (c >= 'a' && c <= 'f')      lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')      lo = c - 'A' + 10;
        else return -EINVAL;
        tmp[n++] = (uint8_t)((hi << 4) | lo);
    }
    if (n != 16) return -EINVAL;
    memcpy(out, tmp, 16);
    return 0;
}

static int cmd_read(const char *path)
{
    struct nebula_io *io = NULL;
    int rc = nebula_io_open(path, false, 0, &io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "open %s: %s\n", path, strerror(-rc));
        return 1;
    }
    struct nebula_mbr mbr;
    rc = nebula_mbr_read(io, &mbr);
    nebula_io_close(io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "MBR invalid or missing: %s\n", strerror(-rc));
        return 1;
    }
    char u[NEBULA_UUID_STR_LEN + 1];
    nebula_uuid_format(mbr.device_uuid, u);
    printf("magic:         %.8s\n",    mbr.magic);
    printf("version:       %u.%u\n",   mbr.version_major, mbr.version_minor);
    printf("uuid:          %s\n",      u);
    printf("capacity:      %lu blocks (%.2f MiB)\n",
           (unsigned long)mbr.device_capacity_blocks,
           (double)mbr.device_capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    printf("sb_head_lba:   %lu\n",     (unsigned long)mbr.superblock_head_lba);
    printf("checksum:      0x%08x\n",  mbr.checksum);
    return 0;
}

static int cmd_write(const char *path, const char *uuid_arg,
                     uint64_t capacity_override)
{
    struct nebula_io *io = NULL;
    int rc = nebula_io_open(path, false, 0, &io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "open %s: %s\n", path, strerror(-rc));
        return 1;
    }

    /* Determine capacity: prefer explicit override, else device size. */
    uint64_t cap_blocks = capacity_override > 0
                          ? capacity_override
                          : nebula_io_capacity_blocks(io);

    /* Determine UUID. */
    uint8_t uuid[16];
    if (!uuid_arg || !strcmp(uuid_arg, "auto")) {
        rc = nebula_uuid_generate(uuid);
        if (rc != 0) {
            fprintf(stderr, "uuid gen: %s\n", strerror(-rc));
            nebula_io_close(io); return 1;
        }
    } else if (parse_uuid(uuid_arg, uuid) < 0) {
        fprintf(stderr, "Invalid --uuid (need 32 hex chars, optional dashes)\n");
        nebula_io_close(io); return 2;
    }

    rc = nebula_mbr_write(io, uuid, cap_blocks);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "MBR write: %s\n", strerror(-rc));
        nebula_io_close(io); return 1;
    }

    /* Keep MBR and SB UUIDs in sync. If a valid head SB is present,
     * rewrite both head and tail with the new UUID and fresh checksum. */
    struct nebula_superblock sb;
    int sb_rc = nebula_superblock_read(io, /*LBA 1*/ 1ULL, &sb);
    if (sb_rc == NEBULA_OK) {
        memcpy(sb.device_uuid, uuid, 16);
        sb.checksum = 0;
        sb.checksum = nebula_superblock_checksum(&sb);

        int w1 = nebula_io_write(io, 1ULL,            1, &sb);
        int w2 = nebula_io_write(io, sb.sb_tail_lba,  1, &sb);
        if (w1 != NEBULA_OK || w2 != NEBULA_OK) {
            fprintf(stderr, "SB sync failed (head=%d tail=%d)\n", w1, w2);
            nebula_io_close(io); return 1;
        }
        printf("Superblocks (head @LBA 1 + tail @LBA %lu) updated to match MBR.\n",
               (unsigned long)sb.sb_tail_lba);
    } else {
        /* No valid SB yet (e.g. labelling a fresh image before mkfs).
         * That's fine; fsck/mount will still work once formatted. */
        NEB_WARN("No valid superblock found; only MBR was updated. "
                 "Run nebula_format to complete the filesystem.");
    }

    (void)nebula_io_flush(io);
    nebula_io_close(io);

    char u[NEBULA_UUID_STR_LEN + 1];
    nebula_uuid_format(uuid, u);
    printf("Label written. uuid=%s capacity=%lu blocks\n", u, (unsigned long)cap_blocks);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    const char *sub  = argv[1];
    const char *path = NULL;
    const char *uuid = NULL;
    uint64_t    cap  = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--uuid") && i + 1 < argc) uuid = argv[++i];
        else if (!strcmp(argv[i], "--capacity-blocks") && i + 1 < argc) {
            cap = strtoull(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }
    if (!path) { usage(argv[0]); return 2; }

    if (!strcmp(sub, "read"))       return cmd_read(path);
    if (!strcmp(sub, "write"))      return cmd_write(path, uuid, cap);

    usage(argv[0]);
    return 2;
}
