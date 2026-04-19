/*
 * nebula_superblock.h - Head and tail superblock writers/readers.
 */
#ifndef NEBULA_SB_PRIV_H
#define NEBULA_SB_PRIV_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

int nebula_superblock_write_both(struct nebula_io *io,
                                 const uint8_t device_uuid[16],
                                 const struct nebula_layout *L);

int nebula_superblock_read(struct nebula_io *io, nebula_lba_t lba,
                           struct nebula_superblock *out);

uint32_t nebula_superblock_checksum(const struct nebula_superblock *sb);

#endif
