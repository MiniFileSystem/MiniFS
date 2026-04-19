/*
 * crc32c.h - CRC-32C (Castagnoli) checksum.
 * Software implementation, slice-by-8 table-based.
 */
#ifndef NEBULA_CRC32C_H
#define NEBULA_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/* Compute CRC32C over buf. Seed normally 0. */
uint32_t crc32c(const void *buf, size_t len);

/* Incremental version if you already have a partial CRC. */
uint32_t crc32c_update(uint32_t crc, const void *buf, size_t len);

#endif
