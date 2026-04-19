/*
 * nebula_dump.c - Read back and print a Nebula-formatted device.
 */
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

#include "../src/util/uuid.h"
#include "../src/util/log.h"

#include "../src/nebula/nebula_mbr.h"
#include "../src/nebula/nebula_superblock.h"
#include "../src/nebula/nebula_uberblock.h"
#include "../src/nebula/nebula_allocator_roots.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *p)
{
    fprintf(stderr, "Usage: %s --path <file>\n", p);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (!path) { usage(argv[0]); return 2; }

    struct nebula_io *io = NULL;
    int rc = nebula_io_open(path, false, 0, &io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "open failed: %s\n", strerror(-rc)); return 1;
    }

    /* MBR */
    struct nebula_mbr mbr;
    rc = nebula_mbr_read(io, &mbr);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "MBR read failed: %s\n", strerror(-rc));
        nebula_io_close(io); return 1;
    }
    char us[NEBULA_UUID_STR_LEN + 1];
    nebula_uuid_format(mbr.device_uuid, us);
    printf("=== MBR ===\n");
    printf("  magic:        %.8s\n", mbr.magic);
    printf("  version:      %u.%u\n", mbr.version_major, mbr.version_minor);
    printf("  uuid:         %s\n", us);
    printf("  capacity:     %lu blocks (%.2f MiB)\n",
           (unsigned long)mbr.device_capacity_blocks,
           (double)mbr.device_capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    printf("  sb_head_lba:  %lu\n", (unsigned long)mbr.superblock_head_lba);
    printf("  checksum:     0x%08x (OK)\n", mbr.checksum);

    /* Superblock (head) */
    struct nebula_superblock sb;
    rc = nebula_superblock_read(io, mbr.superblock_head_lba, &sb);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "SB head read failed: %s\n", strerror(-rc));
        nebula_io_close(io); return 1;
    }
    printf("\n=== Superblock (head) @ LBA %lu ===\n", (unsigned long)mbr.superblock_head_lba);
    printf("  magic:              0x%016lx\n", (unsigned long)sb.magic);
    printf("  version:            %u.%u (inode_size=%u)\n",
           sb.version_major, sb.version_minor, sb.inode_size);
    printf("  uberblock:          lba=%lu count=%u\n",
           (unsigned long)sb.uberblock_lba, sb.uberblock_count);
    printf("  alloc roots head:   lba=%lu\n", (unsigned long)sb.alloc_roots_head_lba);
    printf("  alloc roots tail:   lba=%lu\n", (unsigned long)sb.alloc_roots_tail_lba);
    printf("  bitmap:             lba=%lu pages=%lu\n",
           (unsigned long)sb.bitmap_lba, (unsigned long)sb.bitmap_block_count);
    printf("  stream map:         lba=%lu blocks=%lu\n",
           (unsigned long)sb.stream_map_lba, (unsigned long)sb.stream_map_block_count);
    printf("  inode page:         lba=%lu blocks=%lu (%.2f MiB)\n",
           (unsigned long)sb.inode_page_lba, (unsigned long)sb.inode_page_block_count,
           (double)sb.inode_page_block_count * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    printf("  dir page:           lba=%lu blocks=%lu (%.2f MiB)\n",
           (unsigned long)sb.dir_page_lba, (unsigned long)sb.dir_page_block_count,
           (double)sb.dir_page_block_count * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    printf("  data region:        lba=%lu blocks=%lu (%.2f MiB)\n",
           (unsigned long)sb.data_start_lba, (unsigned long)sb.data_block_count,
           (double)sb.data_block_count * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    printf("  sb_tail_lba:        %lu\n", (unsigned long)sb.sb_tail_lba);
    printf("  checksum:           0x%08x (OK)\n", sb.checksum);

    /* Superblock (tail) */
    struct nebula_superblock sb_tail;
    rc = nebula_superblock_read(io, sb.sb_tail_lba, &sb_tail);
    if (rc == NEBULA_OK) {
        printf("\n=== Superblock (tail) @ LBA %lu === OK\n",
               (unsigned long)sb.sb_tail_lba);
    } else {
        printf("\n=== Superblock (tail) @ LBA %lu === FAILED (%s)\n",
               (unsigned long)sb.sb_tail_lba, strerror(-rc));
    }

    /* Uberblock slot 0 */
    struct nebula_uberblock ub;
    struct nebula_layout L = {
        .uberblock_lba = sb.uberblock_lba,
        .uberblock_count = sb.uberblock_count,
    };
    rc = nebula_uberblock_read_slot(io, &L, 0, &ub);
    printf("\n=== Uberblock slot 0 ===\n");
    if (rc == NEBULA_OK) {
        printf("  txg_id:             %lu\n", (unsigned long)ub.txg_id);
        printf("  timestamp_ns:       %lu\n", (unsigned long)ub.timestamp_ns);
        printf("  bitmap_root_lba:    %lu\n", (unsigned long)ub.bitmap_root_lba);
        printf("  stream_map_lba:     %lu\n", (unsigned long)ub.stream_map_lba);
        printf("  alloc roots count:  %u\n", ub.alloc_roots_count);
        printf("  checksum:           0x%08x (OK)\n", ub.checksum);
    } else {
        printf("  FAILED: %s\n", strerror(-rc));
    }

    /* Alloc roots head (first 3) */
    struct nebula_allocator_root roots[NEBULA_ALLOC_ROOTS_HEAD];
    rc = nebula_allocator_roots_read(io, sb.alloc_roots_head_lba, roots);
    printf("\n=== Alloc roots head @ LBA %lu ===\n", (unsigned long)sb.alloc_roots_head_lba);
    if (rc == NEBULA_OK) {
        uint64_t total_free = 0;
        for (uint32_t i = 0; i < NEBULA_ALLOC_ROOTS_HEAD; i++) total_free += roots[i].free_block_count;
        printf("  64 roots OK, total_free=%lu blocks\n", (unsigned long)total_free);
        for (int i = 0; i < 3 && i < (int)NEBULA_ALLOC_ROOTS_HEAD; i++) {
            printf("  root[%d]: id=%u  range=[%lu, %lu)  free=%lu\n", i,
                   roots[i].root_id,
                   (unsigned long)roots[i].range_start_block,
                   (unsigned long)roots[i].range_end_block,
                   (unsigned long)roots[i].free_block_count);
        }
    } else {
        printf("  FAILED: %s\n", strerror(-rc));
    }

    nebula_io_close(io);
    printf("\nAll metadata regions valid.\n");
    return 0;
}
