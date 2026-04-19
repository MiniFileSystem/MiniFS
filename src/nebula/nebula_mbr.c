/*
 * nebula_mbr.c - MBR (Master Boot Record) writer/reader.
 */
#include "nebula_mbr.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint32_t nebula_mbr_checksum(const struct nebula_mbr *mbr)
{
    struct nebula_mbr tmp = *mbr;
    tmp.checksum = 0;
    return crc32c(&tmp, sizeof(tmp));
}

int nebula_mbr_write(struct nebula_io *io,
                     const uint8_t device_uuid[16],
                     uint64_t capacity_blocks)
{
    if (!io || !device_uuid) return -EINVAL;

    struct nebula_mbr *mbr = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!mbr) return -ENOMEM;
    memset(mbr, 0, NEBULA_BLOCK_SIZE);

    memcpy(mbr->magic, NEBULA_MAGIC_MBR_STR, 8);
    mbr->version_major = NEBULA_VERSION_MAJOR;
    mbr->version_minor = NEBULA_VERSION_MINOR;
    memcpy(mbr->device_uuid, device_uuid, 16);
    mbr->device_capacity_blocks = capacity_blocks;
    mbr->superblock_head_lba    = NEBULA_LBA_SB_HEAD;
    mbr->checksum = nebula_mbr_checksum(mbr);

    int rc = nebula_io_write(io, NEBULA_LBA_MBR, 1, mbr);
    free(mbr);
    if (rc != NEBULA_OK) {
        NEB_ERR("MBR write failed: %d", rc);
    }
    return rc;
}

int nebula_mbr_read(struct nebula_io *io, struct nebula_mbr *out)
{
    if (!io || !out) return -EINVAL;

    struct nebula_mbr *buf = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    int rc = nebula_io_read(io, NEBULA_LBA_MBR, 1, buf);
    if (rc != NEBULA_OK) { free(buf); return rc; }

    if (memcmp(buf->magic, NEBULA_MAGIC_MBR_STR, 8) != 0) {
        free(buf); return -EINVAL;
    }
    uint32_t want = buf->checksum;
    uint32_t got  = nebula_mbr_checksum(buf);
    if (want != got) {
        NEB_ERR("MBR checksum mismatch: want=%08x got=%08x", want, got);
        free(buf); return -EIO;
    }
    *out = *buf;
    free(buf);
    return NEBULA_OK;
}
