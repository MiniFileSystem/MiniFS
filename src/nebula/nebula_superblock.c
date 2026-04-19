/*
 * nebula_superblock.c - Head and tail superblock writers/readers.
 */
#include "nebula_superblock.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint32_t nebula_superblock_checksum(const struct nebula_superblock *sb)
{
    struct nebula_superblock tmp = *sb;
    tmp.checksum = 0;
    return crc32c(&tmp, sizeof(tmp));
}

int nebula_superblock_write_both(struct nebula_io *io,
                                 const uint8_t device_uuid[16],
                                 const struct nebula_layout *L)
{
    if (!io || !device_uuid || !L) return -EINVAL;

    struct nebula_superblock *sb = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!sb) return -ENOMEM;
    memset(sb, 0, NEBULA_BLOCK_SIZE);

    sb->magic                    = NEBULA_MAGIC_SB;
    sb->version_major            = NEBULA_VERSION_MAJOR;
    sb->version_minor            = NEBULA_VERSION_MINOR;
    sb->inode_size               = NEBULA_INODE_SIZE;
    memcpy(sb->device_uuid, device_uuid, 16);
    sb->device_capacity_blocks   = L->capacity_blocks;

    sb->uberblock_lba            = L->uberblock_lba;
    sb->uberblock_count          = (uint32_t)L->uberblock_count;

    sb->alloc_roots_head_lba     = L->alloc_roots_head_lba;
    sb->alloc_roots_tail_lba     = L->alloc_roots_tail_lba;

    sb->bitmap_lba               = L->bitmap_lba;
    sb->bitmap_block_count       = L->bitmap_block_count;

    sb->stream_map_lba           = L->stream_map_lba;
    sb->stream_map_block_count   = L->stream_map_block_count;

    sb->inode_page_lba           = L->inode_page_lba;
    sb->inode_page_block_count   = L->inode_page_block_count;

    sb->dir_page_lba             = L->dir_page_lba;
    sb->dir_page_block_count     = L->dir_page_block_count;

    sb->data_start_lba           = L->data_start_lba;
    sb->data_block_count         = L->data_block_count;

    sb->sb_tail_lba              = L->sb_tail_lba;
    sb->checksum                 = nebula_superblock_checksum(sb);

    int rc = nebula_io_write(io, L->sb_head_lba, 1, sb);
    if (rc != NEBULA_OK) { free(sb); return rc; }

    rc = nebula_io_write(io, L->sb_tail_lba, 1, sb);
    free(sb);
    return rc;
}

int nebula_superblock_read(struct nebula_io *io, nebula_lba_t lba,
                           struct nebula_superblock *out)
{
    if (!io || !out) return -EINVAL;

    struct nebula_superblock *buf = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    int rc = nebula_io_read(io, lba, 1, buf);
    if (rc != NEBULA_OK) { free(buf); return rc; }

    if (buf->magic != NEBULA_MAGIC_SB) { free(buf); return -EINVAL; }
    uint32_t want = buf->checksum;
    uint32_t got  = nebula_superblock_checksum(buf);
    if (want != got) {
        NEB_ERR("Superblock @lba %lu checksum mismatch", (unsigned long)lba);
        free(buf); return -EIO;
    }
    *out = *buf;
    free(buf);
    return NEBULA_OK;
}
