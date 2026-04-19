/*
 * nebula_mbr.h - MBR read/write.
 */
#ifndef NEBULA_MBR_PRIV_H
#define NEBULA_MBR_PRIV_H

#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"

/* Write the MBR at LBA 0. */
int nebula_mbr_write(struct nebula_io *io,
                     const uint8_t device_uuid[16],
                     uint64_t device_capacity_blocks);

/* Read and validate the MBR from LBA 0. */
int nebula_mbr_read(struct nebula_io *io, struct nebula_mbr *out);

/* Compute checksum over mbr with its checksum field zeroed. */
uint32_t nebula_mbr_checksum(const struct nebula_mbr *mbr);

#endif
