/*
 * nebula_types.h - Global constants and error codes.
 */
#ifndef NEBULA_TYPES_H
#define NEBULA_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>

/* Block sizes */
#define NEBULA_BLOCK_SIZE        4096U
#define NEBULA_BLOCK_SHIFT       12
#define NEBULA_SECTOR_SIZE       512U

/* All LBAs in Nebula are 4 KB block numbers (not 512 B sectors). */
typedef uint64_t nebula_lba_t;

/* Chunk size = 128 MiB (for future use) */
#define NEBULA_CHUNK_SIZE        (128ULL * 1024 * 1024)
#define NEBULA_CHUNK_BLOCKS      (NEBULA_CHUNK_SIZE / NEBULA_BLOCK_SIZE)  /* 32768 */

/* Inode */
#define NEBULA_INODE_SIZE        4096U
#define NEBULA_ROOT_INODE_NUM    0ULL

/* Version */
#define NEBULA_VERSION_MAJOR     1
#define NEBULA_VERSION_MINOR     0

/* Allocator */
#define NEBULA_UBERBLOCK_SLOTS   128U
#define NEBULA_ALLOC_ROOTS_TOTAL 128U
#define NEBULA_ALLOC_ROOTS_HEAD  64U
#define NEBULA_ALLOC_ROOTS_TAIL  64U

/* Minimum device size: 256 MiB. Smaller can't fit inode+dir pages. */
#define NEBULA_MIN_DEVICE_BYTES  (256ULL * 1024 * 1024)

/* Error codes: 0 = OK, negative errno-style. */
#define NEBULA_OK                0

#endif /* NEBULA_TYPES_H */
