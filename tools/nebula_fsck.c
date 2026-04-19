/*
 * nebula_fsck.c - Validate every on-disk structure on a Nebula device.
 *
 * Exit codes:
 *   0 = clean (all checks pass)
 *   1 = warnings (e.g. tail SB missing but head OK)
 *   2 = errors (unmountable)
 *   3 = usage error
 */
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"

#include "../src/util/crc32c.h"
#include "../src/util/uuid.h"
#include "../src/util/log.h"

#include "../src/nebula/nebula_mbr.h"
#include "../src/nebula/nebula_superblock.h"
#include "../src/nebula/nebula_uberblock.h"
#include "../src/nebula/nebula_allocator_roots.h"
#include "../src/nebula/nebula_inode_init.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(fmt, ...) do { printf("  [ OK ] " fmt "\n", ##__VA_ARGS__); } while (0)
#define WARN(fmt, ...) do { printf("  [WARN] " fmt "\n", ##__VA_ARGS__); warns++; } while (0)
#define FAIL(fmt, ...) do { printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); errs++;  } while (0)

static int warns = 0;
static int errs  = 0;

/* ---------- individual checks ---------- */

static int check_mbr(struct nebula_io *io, struct nebula_mbr *out)
{
    printf("[1/6] MBR @ LBA 0\n");
    int rc = nebula_mbr_read(io, out);
    if (rc != NEBULA_OK) {
        FAIL("read failed: %s", strerror(-rc));
        return rc;
    }
    PASS("magic=%.6s uuid-valid checksum=0x%08x", out->magic, out->checksum);
    if (out->device_capacity_blocks != nebula_io_capacity_blocks(io)) {
        WARN("MBR capacity %lu != device size %lu blocks",
             (unsigned long)out->device_capacity_blocks,
             (unsigned long)nebula_io_capacity_blocks(io));
    }
    return NEBULA_OK;
}

static int check_superblocks(struct nebula_io *io, const struct nebula_mbr *mbr,
                             struct nebula_superblock *sb_out)
{
    printf("[2/6] Superblocks\n");
    struct nebula_superblock head, tail;
    int rc_h = nebula_superblock_read(io, mbr->superblock_head_lba, &head);
    if (rc_h != NEBULA_OK) {
        FAIL("head @ LBA %lu: %s",
             (unsigned long)mbr->superblock_head_lba, strerror(-rc_h));
    } else {
        PASS("head @ LBA %lu checksum=0x%08x",
             (unsigned long)mbr->superblock_head_lba, head.checksum);
    }

    int rc_t = -1;
    if (rc_h == NEBULA_OK) {
        rc_t = nebula_superblock_read(io, head.sb_tail_lba, &tail);
        if (rc_t != NEBULA_OK) {
            WARN("tail @ LBA %lu: %s", (unsigned long)head.sb_tail_lba,
                 strerror(-rc_t));
        } else {
            PASS("tail @ LBA %lu checksum=0x%08x",
                 (unsigned long)head.sb_tail_lba, tail.checksum);
            /* Compare critical fields */
            if (memcmp(head.device_uuid, tail.device_uuid, 16) != 0) {
                FAIL("head/tail UUID mismatch");
            }
            if (head.uberblock_lba != tail.uberblock_lba ||
                head.bitmap_lba    != tail.bitmap_lba) {
                FAIL("head/tail region pointers differ");
            }
        }
    }

    if (rc_h == NEBULA_OK) {
        /* Cross-check SB UUID vs MBR UUID */
        if (memcmp(head.device_uuid, mbr->device_uuid, 16) != 0) {
            FAIL("SB UUID differs from MBR UUID");
        } else {
            PASS("SB UUID matches MBR");
        }
        *sb_out = head;
        return NEBULA_OK;
    }
    return rc_h;
}

static int check_uberblocks(struct nebula_io *io, const struct nebula_superblock *sb)
{
    printf("[3/6] Uberblocks (%u slots)\n", sb->uberblock_count);
    struct nebula_layout L = {
        .uberblock_lba = sb->uberblock_lba,
        .uberblock_count = sb->uberblock_count,
    };
    uint32_t valid = 0;
    uint64_t max_txg = 0;
    int      max_slot = -1;
    for (uint32_t slot = 0; slot < sb->uberblock_count; slot++) {
        struct nebula_uberblock ub;
        int rc = nebula_uberblock_read_slot(io, &L, slot, &ub);
        if (rc == NEBULA_OK) {
            valid++;
            if (max_slot < 0 || ub.txg_id >= max_txg) {
                max_txg  = ub.txg_id;
                max_slot = (int)slot;
            }
        } else if (rc == -ENOENT) {
            /* empty slot, fine */
        } else {
            WARN("slot %u: %s", slot, strerror(-rc));
        }
    }
    if (valid == 0) {
        FAIL("no valid uberblock found");
        return -EIO;
    }
    PASS("%u/%u valid; latest=slot%d txg=%lu",
         valid, sb->uberblock_count, max_slot, (unsigned long)max_txg);
    return NEBULA_OK;
}

static int check_alloc_roots(struct nebula_io *io, const struct nebula_superblock *sb)
{
    printf("[4/6] Allocator roots\n");
    struct nebula_allocator_root rh[NEBULA_ALLOC_ROOTS_HEAD];
    struct nebula_allocator_root rt[NEBULA_ALLOC_ROOTS_TAIL];
    int rc_h = nebula_allocator_roots_read(io, sb->alloc_roots_head_lba, rh);
    int rc_t = nebula_allocator_roots_read(io, sb->alloc_roots_tail_lba, rt);
    if (rc_h != NEBULA_OK || rc_t != NEBULA_OK) {
        FAIL("read failed (head=%d, tail=%d)", rc_h, rc_t);
        return -EIO;
    }
    uint32_t bad = 0;
    uint64_t total_free = 0;
    for (uint32_t i = 0; i < NEBULA_ALLOC_ROOTS_HEAD; i++) {
        if (rh[i].checksum != nebula_allocator_root_checksum(&rh[i])) bad++;
        else total_free += rh[i].free_block_count;
    }
    for (uint32_t i = 0; i < NEBULA_ALLOC_ROOTS_TAIL; i++) {
        if (rt[i].checksum != nebula_allocator_root_checksum(&rt[i])) bad++;
        else total_free += rt[i].free_block_count;
    }
    if (bad) {
        FAIL("%u roots failed checksum", bad);
        return -EIO;
    }
    PASS("128 roots valid; total_free=%lu blocks (%.2f MiB)",
         (unsigned long)total_free,
         (double)total_free * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));
    return NEBULA_OK;
}

static int check_root_inode(struct nebula_io *io, const struct nebula_superblock *sb)
{
    printf("[5/6] Root inode\n");
    struct nebula_inode *ino = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!ino) return -ENOMEM;

    int rc = nebula_io_read(io, sb->inode_page_lba, 1, ino);
    if (rc != NEBULA_OK) {
        FAIL("read failed: %s", strerror(-rc));
        free(ino); return rc;
    }
    uint32_t want = ino->checksum;
    uint32_t got  = nebula_inode_checksum(ino);
    if (want != got) {
        FAIL("checksum mismatch: want=0x%08x got=0x%08x", want, got);
        free(ino); return -EIO;
    }
    if (ino->type != NEBULA_INODE_TYPE_DIR) {
        FAIL("root inode type=%u, expected DIR(%u)", ino->type, NEBULA_INODE_TYPE_DIR);
        free(ino); return -EIO;
    }
    PASS("inode #%lu type=DIR checksum=0x%08x",
         (unsigned long)ino->inode_num, ino->checksum);
    free(ino);
    return NEBULA_OK;
}

static int check_dir_header(struct nebula_io *io, const struct nebula_superblock *sb)
{
    printf("[6/6] Directory page header\n");
    struct nebula_dir_page_header *h = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!h) return -ENOMEM;
    int rc = nebula_io_read(io, sb->dir_page_lba, 1, h);
    if (rc != NEBULA_OK) {
        FAIL("read failed: %s", strerror(-rc));
        free(h); return rc;
    }
    if (h->magic != NEBULA_MAGIC_DIR_PAGE) {
        FAIL("bad magic 0x%016lx", (unsigned long)h->magic);
        free(h); return -EIO;
    }
    struct nebula_dir_page_header tmp = *h;
    tmp.checksum = 0;
    uint32_t got = crc32c(&tmp, sizeof(tmp));
    if (got != h->checksum) {
        FAIL("checksum mismatch want=0x%08x got=0x%08x", h->checksum, got);
        free(h); return -EIO;
    }
    PASS("entries=%lu/%lu (entry_size=%u)",
         (unsigned long)h->num_entries, (unsigned long)h->capacity, h->entry_size);
    free(h);
    return NEBULA_OK;
}

/* ---------- main ---------- */

static void usage(const char *p) { fprintf(stderr, "Usage: %s --path <file>\n", p); }

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else { usage(argv[0]); return 3; }
    }
    if (!path) { usage(argv[0]); return 3; }

    struct nebula_io *io = NULL;
    int rc = nebula_io_open(path, false, 0, &io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "open %s: %s\n", path, strerror(-rc)); return 2;
    }

    struct nebula_mbr mbr;
    struct nebula_superblock sb;

    printf("fsck: %s\n", path);
    printf("capacity: %lu blocks (%.2f MiB)\n\n",
           (unsigned long)nebula_io_capacity_blocks(io),
           (double)nebula_io_capacity_blocks(io) * NEBULA_BLOCK_SIZE / (1024.0*1024.0));

    if (check_mbr(io, &mbr) == NEBULA_OK)
        if (check_superblocks(io, &mbr, &sb) == NEBULA_OK) {
            check_uberblocks(io, &sb);
            check_alloc_roots(io, &sb);
            check_root_inode(io, &sb);
            check_dir_header(io, &sb);
        }

    nebula_io_close(io);

    printf("\nSummary: %d error(s), %d warning(s)\n", errs, warns);
    if (errs)  return 2;
    if (warns) return 1;
    printf("CLEAN\n");
    return 0;
}
