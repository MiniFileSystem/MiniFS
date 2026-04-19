/*
 * nebula_mount.c - Device mount lifecycle.
 *
 * Steps:
 *   1. Open device (POSIX backend).
 *   2. Read MBR; validate magic + checksum.
 *   3. Read head superblock; if invalid, try tail.
 *   4. Scan all N uberblock slots; pick highest valid TXG.
 *   5. Load bitmap pages into hierarchical bitmap.
 *   6. Stream map replay (no-op for Milestone 2; records are zeroed).
 */
#include "nebula_mount.h"
#include "nebula_mbr.h"
#include "nebula_superblock.h"
#include "nebula_uberblock.h"
#include "nebula_hier_bitmap.h"
#include "../util/log.h"
#include "../util/uuid.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pick_latest_uberblock(struct nebula_io *io,
                                 const struct nebula_superblock *sb,
                                 struct nebula_uberblock *out_ub,
                                 uint32_t *out_slot)
{
    struct nebula_layout L = {
        .uberblock_lba   = sb->uberblock_lba,
        .uberblock_count = sb->uberblock_count,
    };
    int  best_slot = -1;
    uint64_t best_txg = 0;
    struct nebula_uberblock best_ub;
    memset(&best_ub, 0, sizeof(best_ub));

    for (uint32_t slot = 0; slot < sb->uberblock_count; slot++) {
        struct nebula_uberblock ub;
        int rc = nebula_uberblock_read_slot(io, &L, slot, &ub);
        if (rc != NEBULA_OK) continue;
        if (best_slot < 0 || ub.txg_id >= best_txg) {
            best_txg  = ub.txg_id;
            best_slot = (int)slot;
            best_ub   = ub;
        }
    }
    if (best_slot < 0) return -EIO;
    *out_ub   = best_ub;
    *out_slot = (uint32_t)best_slot;
    return NEBULA_OK;
}

int nebula_mount_open(const char *path, struct nebula_mount **out)
{
    if (!path || !out) return -EINVAL;

    struct nebula_mount *m = calloc(1, sizeof(*m));
    if (!m) return -ENOMEM;

    int rc = nebula_io_open(path, false, 0, &m->io);
    if (rc != NEBULA_OK) { free(m); return rc; }

    /* Step 2: MBR */
    rc = nebula_mbr_read(m->io, &m->mbr);
    if (rc != NEBULA_OK) {
        NEB_ERR("mount: invalid MBR: %s", strerror(-rc));
        goto fail;
    }

    /* Step 3: Superblock (head, fallback to tail) */
    rc = nebula_superblock_read(m->io, m->mbr.superblock_head_lba, &m->sb);
    if (rc != NEBULA_OK) {
        NEB_WARN("mount: head SB bad (%s); trying tail", strerror(-rc));
        /* We don't know tail LBA without a good SB. Assume capacity-1. */
        nebula_lba_t tail = nebula_io_capacity_blocks(m->io) - 1;
        rc = nebula_superblock_read(m->io, tail, &m->sb);
        if (rc != NEBULA_OK) {
            NEB_ERR("mount: tail SB also bad: %s", strerror(-rc));
            goto fail;
        }
        NEB_WARN("mount: using tail SB (head is corrupt)");
    }

    /* Cross-check UUIDs */
    if (memcmp(m->mbr.device_uuid, m->sb.device_uuid, 16) != 0) {
        NEB_ERR("mount: MBR/SB UUID mismatch");
        rc = -EIO; goto fail;
    }

    /* Step 4: Uberblock */
    rc = pick_latest_uberblock(m->io, &m->sb, &m->current_ub, &m->current_ub_slot);
    if (rc != NEBULA_OK) {
        NEB_ERR("mount: no valid uberblock");
        goto fail;
    }

    /* Step 5: Hierarchical bitmap */
    rc = nebula_hbm_load(m->io, m->sb.bitmap_lba, m->sb.bitmap_block_count,
                         m->sb.device_capacity_blocks,
                         &m->bitmap);
    if (rc != NEBULA_OK) {
        NEB_ERR("mount: bitmap load: %s", strerror(-rc));
        goto fail;
    }

    /* Step 6: Stream replay - deferred to Milestone 3. */

    *out = m;
    return NEBULA_OK;

fail:
    nebula_mount_unmount(m);
    return rc;
}

void nebula_mount_unmount(struct nebula_mount *m)
{
    if (!m) return;
    if (m->bitmap) nebula_hbm_free(m->bitmap);
    if (m->io)     nebula_io_close(m->io);
    free(m);
}

void nebula_mount_print(const struct nebula_mount *m)
{
    if (!m) return;
    char u[NEBULA_UUID_STR_LEN + 1];
    nebula_uuid_format(m->mbr.device_uuid, u);
    printf("MOUNTED\n");
    printf("  uuid:            %s\n", u);
    printf("  version:         %u.%u\n", m->mbr.version_major, m->mbr.version_minor);
    printf("  capacity:        %lu blocks (%.2f MiB)\n",
           (unsigned long)m->mbr.device_capacity_blocks,
           (double)m->mbr.device_capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0*1024.0));
    printf("  uberblock slot:  %u  (txg=%lu)\n",
           m->current_ub_slot, (unsigned long)m->current_ub.txg_id);
    printf("  bitmap_root:     LBA %lu\n", (unsigned long)m->current_ub.bitmap_root_lba);
    printf("  stream_map:      LBA %lu (head_off=%lu)\n",
           (unsigned long)m->current_ub.stream_map_lba,
           (unsigned long)m->current_ub.stream_map_head_offset);
    nebula_hbm_print_summary(m->bitmap);
}
