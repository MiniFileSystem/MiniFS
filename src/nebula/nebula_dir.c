/*
 * nebula_dir.c - Directory entry add / lookup / remove helpers.
 *
 * Scans the flat directory entry array stored in the dir region.
 * All operations are O(N) linear scan for now (no hash table yet).
 */
#include "nebula_dir.h"
#include "nebula_mount.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------- helpers -------------------------------------------------- */

static uint64_t fnv1a_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint64_t)(uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
 * Read the content block that holds slot `slot_idx` into `buf` (4 KiB).
 * Returns the LBA of that block and a pointer to the entry within it.
 */
static nebula_lba_t slot_lba(const struct nebula_mount *m, uint64_t slot_idx)
{
    return (nebula_lba_t)(m->sb.dir_page_lba + 1 +
                          slot_idx / NEBULA_DIR_ENTRIES_PER_BLOCK);
}

static uint32_t slot_within_block(uint64_t slot_idx)
{
    return (uint32_t)(slot_idx % NEBULA_DIR_ENTRIES_PER_BLOCK);
}

/* Update num_entries in the header block by delta (+1 or -1). */
static int dir_header_update(struct nebula_mount *m, int64_t delta)
{
    struct nebula_dir_page_header *hdr =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!hdr) return -ENOMEM;

    int rc = nebula_io_read(m->io, m->sb.dir_page_lba, 1, hdr);
    if (rc != NEBULA_OK) { free(hdr); return rc; }

    if (delta > 0)
        hdr->num_entries += (uint64_t)delta;
    else if ((uint64_t)(-delta) <= hdr->num_entries)
        hdr->num_entries -= (uint64_t)(-delta);

    /* Recompute checksum */
    {
        struct nebula_dir_page_header tmp = *hdr;
        tmp.checksum = 0;
        hdr->checksum = crc32c(&tmp, sizeof(tmp));
    }

    rc = nebula_io_write(m->io, m->sb.dir_page_lba, 1, hdr);
    free(hdr);
    return rc;
}

/* ---------- public API ----------------------------------------------- */

int nebula_dir_add(struct nebula_mount *m, const char *name,
                   uint64_t inode_num, uint16_t type_flag)
{
    if (!m || !name || name[0] == '\0') return -EINVAL;
    size_t nlen = strlen(name);
    if (nlen >= NEBULA_DIR_NAME_MAX) return -ENAMETOOLONG;

    /* Total content slots available */
    uint64_t capacity =
        (m->sb.dir_page_block_count - 1) * NEBULA_DIR_ENTRIES_PER_BLOCK;

    uint64_t hash = fnv1a_hash(name);

    void *blk = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!blk) return -ENOMEM;

    nebula_lba_t last_lba  = (nebula_lba_t)-1;
    int64_t      free_slot = -1;

    /* Linear scan for duplicate check and free slot */
    for (uint64_t s = 0; s < capacity; s++) {
        nebula_lba_t lba = slot_lba(m, s);

        if (lba != last_lba) {
            int rc = nebula_io_read(m->io, lba, 1, blk);
            if (rc != NEBULA_OK) { free(blk); return rc; }
            last_lba = lba;
        }

        struct nebula_dir_entry *de =
            (struct nebula_dir_entry *)blk + slot_within_block(s);

        if (!(de->flags & NEBULA_DIR_FLAG_USED)) {
            if (free_slot < 0) free_slot = (int64_t)s;
            continue;
        }

        if (de->hash == hash && de->name_len == (uint16_t)nlen &&
            memcmp(de->name, name, nlen) == 0) {
            free(blk);
            return -EEXIST;
        }
    }

    if (free_slot < 0) { free(blk); return -ENOSPC; }

    /* Write the new entry */
    nebula_lba_t wlba = slot_lba(m, (uint64_t)free_slot);

    if (wlba != last_lba) {
        int rc = nebula_io_read(m->io, wlba, 1, blk);
        if (rc != NEBULA_OK) { free(blk); return rc; }
    }

    struct nebula_dir_entry *de =
        (struct nebula_dir_entry *)blk + slot_within_block((uint64_t)free_slot);
    memset(de, 0, sizeof(*de));
    de->hash      = hash;
    de->inode_num = inode_num;
    de->name_len  = (uint16_t)nlen;
    de->flags     = NEBULA_DIR_FLAG_USED | type_flag;
    memcpy(de->name, name, nlen);

    int rc = nebula_io_write(m->io, wlba, 1, blk);
    free(blk);
    if (rc != NEBULA_OK) return rc;

    return dir_header_update(m, +1);
}

int nebula_dir_lookup(struct nebula_mount *m, const char *name,
                      uint64_t *out_inode_num)
{
    if (!m || !name || !out_inode_num) return -EINVAL;
    size_t nlen = strlen(name);
    if (nlen >= NEBULA_DIR_NAME_MAX) return -ENAMETOOLONG;

    uint64_t capacity =
        (m->sb.dir_page_block_count - 1) * NEBULA_DIR_ENTRIES_PER_BLOCK;
    uint64_t hash = fnv1a_hash(name);

    void *blk = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!blk) return -ENOMEM;

    nebula_lba_t last_lba = (nebula_lba_t)-1;

    for (uint64_t s = 0; s < capacity; s++) {
        nebula_lba_t lba = slot_lba(m, s);

        if (lba != last_lba) {
            int rc = nebula_io_read(m->io, lba, 1, blk);
            if (rc != NEBULA_OK) { free(blk); return rc; }
            last_lba = lba;
        }

        const struct nebula_dir_entry *de =
            (const struct nebula_dir_entry *)blk + slot_within_block(s);

        if (!(de->flags & NEBULA_DIR_FLAG_USED)) continue;

        if (de->hash == hash && de->name_len == (uint16_t)nlen &&
            memcmp(de->name, name, nlen) == 0) {
            *out_inode_num = de->inode_num;
            free(blk);
            return NEBULA_OK;
        }
    }

    free(blk);
    return -ENOENT;
}

int nebula_dir_remove(struct nebula_mount *m, const char *name)
{
    if (!m || !name) return -EINVAL;
    size_t nlen = strlen(name);
    if (nlen >= NEBULA_DIR_NAME_MAX) return -ENAMETOOLONG;

    uint64_t capacity =
        (m->sb.dir_page_block_count - 1) * NEBULA_DIR_ENTRIES_PER_BLOCK;
    uint64_t hash = fnv1a_hash(name);

    void *blk = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!blk) return -ENOMEM;

    nebula_lba_t last_lba = (nebula_lba_t)-1;

    for (uint64_t s = 0; s < capacity; s++) {
        nebula_lba_t lba = slot_lba(m, s);

        if (lba != last_lba) {
            int rc = nebula_io_read(m->io, lba, 1, blk);
            if (rc != NEBULA_OK) { free(blk); return rc; }
            last_lba = lba;
        }

        struct nebula_dir_entry *de =
            (struct nebula_dir_entry *)blk + slot_within_block(s);

        if (!(de->flags & NEBULA_DIR_FLAG_USED)) continue;

        if (de->hash == hash && de->name_len == (uint16_t)nlen &&
            memcmp(de->name, name, nlen) == 0) {
            memset(de, 0, sizeof(*de));   /* clears USED flag */
            int rc = nebula_io_write(m->io, lba, 1, blk);
            free(blk);
            if (rc != NEBULA_OK) return rc;
            return dir_header_update(m, -1);
        }
    }

    free(blk);
    return -ENOENT;
}

int nebula_dir_list(struct nebula_mount *m,
                    int (*cb)(const char *name, uint64_t inode_num,
                              uint16_t flags, void *ud),
                    void *ud)
{
    if (!m || !cb) return -EINVAL;

    uint64_t capacity =
        (m->sb.dir_page_block_count - 1) * NEBULA_DIR_ENTRIES_PER_BLOCK;

    void *blk = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!blk) return -ENOMEM;

    nebula_lba_t last_lba = (nebula_lba_t)-1;

    for (uint64_t s = 0; s < capacity; s++) {
        nebula_lba_t lba = slot_lba(m, s);

        if (lba != last_lba) {
            int rc = nebula_io_read(m->io, lba, 1, blk);
            if (rc != NEBULA_OK) { free(blk); return rc; }
            last_lba = lba;
        }

        const struct nebula_dir_entry *de =
            (const struct nebula_dir_entry *)blk + slot_within_block(s);

        if (!(de->flags & NEBULA_DIR_FLAG_USED)) continue;

        /* NUL-terminate for the callback */
        char name[NEBULA_DIR_NAME_MAX + 1];
        uint16_t nlen = de->name_len < NEBULA_DIR_NAME_MAX
                        ? de->name_len : NEBULA_DIR_NAME_MAX;
        memcpy(name, de->name, nlen);
        name[nlen] = '\0';

        int ret = cb(name, de->inode_num, de->flags, ud);
        if (ret != 0) { free(blk); return ret; }
    }

    free(blk);
    return NEBULA_OK;
}
