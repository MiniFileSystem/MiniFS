/*
 * nebula_format.h - On-disk structure definitions for Nebula FS.
 *
 * All structures are packed, little-endian, and size-asserted.
 * Checksums are CRC32C computed over the structure with the checksum
 * field set to zero.
 */
#ifndef NEBULA_FORMAT_H
#define NEBULA_FORMAT_H

#include "nebula_types.h"

/* _Static_assert is C11; static_assert is C++11.  Bridge the two. */
#ifdef __cplusplus
#  define NEBULA_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#  define NEBULA_STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#endif

/* ---------- Magic numbers ---------- */
#define NEBULA_MAGIC_MBR_STR     "NEBULA\0"                 /* 8 bytes incl NUL */
#define NEBULA_MAGIC_SB          0x4E45425553420001ULL      /* NEBUSB + v1 */
#define NEBULA_MAGIC_UB          0x4E4542555542424CULL      /* NEBUUBBL */
#define NEBULA_MAGIC_INODE       0x4E4549444E4F4445ULL      /* NEIDNODE */
#define NEBULA_MAGIC_DIR_PAGE    0x4E45444952504147ULL      /* NEDIRPAG */
#define NEBULA_MAGIC_BITMAP_PAGE 0x4E45424D50414745ULL      /* NEBMPAGE */
#define NEBULA_MAGIC_ALLOC_ROOT  0x4E4541524F4F5421ULL      /* NEAROOT! */

/* ---------- Fixed LBA positions ---------- */
#define NEBULA_LBA_MBR             0ULL
#define NEBULA_LBA_SB_HEAD         1ULL
#define NEBULA_LBA_UBERBLOCK       2ULL   /* + 128 slots */
#define NEBULA_LBA_ALLOC_ROOTS_H   (NEBULA_LBA_UBERBLOCK + NEBULA_UBERBLOCK_SLOTS)
#define NEBULA_LBA_AFTER_METAHEAD  (NEBULA_LBA_ALLOC_ROOTS_H + 1)

/* ==================================================================
 * MBR - 4 KB at LBA 0
 * ================================================================== */
struct __attribute__((packed)) nebula_mbr {
    char     magic[8];                /* "NEBULA\0\0" */
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t _pad0;
    uint8_t  device_uuid[16];
    uint64_t device_capacity_blocks;  /* 4 KB blocks */
    uint64_t superblock_head_lba;
    uint32_t checksum;                /* CRC32C, this field=0 when computing */
    uint8_t  reserved[NEBULA_BLOCK_SIZE - 52];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_mbr) == NEBULA_BLOCK_SIZE, "MBR must be 4KB");

/* ==================================================================
 * Superblock - 4 KB, head at LBA 1, tail at end of device
 * ================================================================== */
struct __attribute__((packed)) nebula_superblock {
    uint64_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t inode_size;              /* 4096 */

    uint8_t  device_uuid[16];
    uint64_t device_capacity_blocks;

    uint64_t uberblock_lba;
    uint32_t uberblock_count;         /* 128 */
    uint32_t _pad0;

    uint64_t alloc_roots_head_lba;
    uint64_t alloc_roots_tail_lba;

    uint64_t bitmap_lba;
    uint64_t bitmap_block_count;

    uint64_t stream_map_lba;
    uint64_t stream_map_block_count;

    uint64_t inode_page_lba;
    uint64_t inode_page_block_count;

    uint64_t dir_page_lba;
    uint64_t dir_page_block_count;

    uint64_t data_start_lba;
    uint64_t data_block_count;

    uint64_t sb_tail_lba;

    uint32_t checksum;
    uint32_t _pad1;
    uint8_t  reserved[NEBULA_BLOCK_SIZE - 168];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_superblock) == NEBULA_BLOCK_SIZE, "SB must be 4KB");

/* ==================================================================
 * Uberblock - 4 KB per slot, 128 slots
 * ================================================================== */
struct __attribute__((packed)) nebula_uberblock {
    uint64_t magic;
    uint64_t txg_id;
    uint64_t timestamp_ns;
    uint64_t bitmap_root_lba;
    uint64_t stream_map_lba;
    uint64_t stream_map_head_offset;  /* bytes written into stream map */
    uint32_t alloc_roots_count;
    uint32_t checksum;
    uint8_t  reserved[NEBULA_BLOCK_SIZE - 56];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_uberblock) == NEBULA_BLOCK_SIZE, "UB must be 4KB");

/* ==================================================================
 * Allocator root - 64 bytes, 64 per 4 KB block
 * ================================================================== */
struct __attribute__((packed)) nebula_allocator_root {
    uint32_t root_id;
    uint32_t _pad0;
    uint64_t range_start_block;
    uint64_t range_end_block;         /* exclusive */
    uint64_t free_block_count;
    uint64_t last_alloc_hint;
    uint64_t bitmap_region_lba;
    uint64_t stream_map_region_lba;
    uint32_t checksum;
    uint8_t  reserved[64 - 60];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_allocator_root) == 64, "AR must be 64B");

#define NEBULA_ALLOC_ROOTS_PER_BLOCK (NEBULA_BLOCK_SIZE / sizeof(struct nebula_allocator_root))
NEBULA_STATIC_ASSERT(NEBULA_ALLOC_ROOTS_PER_BLOCK == NEBULA_ALLOC_ROOTS_HEAD,
               "64 roots must fit in 1 block");

/* ==================================================================
 * Bitmap page - 4 KB tracks 32768 blocks (1 bit each)
 * ================================================================== */
#define NEBULA_BITMAP_BITS_PER_PAGE (NEBULA_BLOCK_SIZE * 8U)  /* 32768 */

struct __attribute__((packed)) nebula_bitmap_page {
    uint8_t bits[NEBULA_BLOCK_SIZE];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_bitmap_page) == NEBULA_BLOCK_SIZE, "BM page 4KB");

/* ==================================================================
 * Inode - 4 KB.  First 128 B = attributes, rest = extent map.
 * ================================================================== */
#define NEBULA_INODE_ATTR_SIZE     128
#define NEBULA_EXTENT_ENTRY_SIZE   16
#define NEBULA_EXTENTS_PER_INODE   ((NEBULA_BLOCK_SIZE - NEBULA_INODE_ATTR_SIZE) / \
                                    NEBULA_EXTENT_ENTRY_SIZE)  /* 248 */

#define NEBULA_INODE_TYPE_FREE     0
#define NEBULA_INODE_TYPE_FILE     1
#define NEBULA_INODE_TYPE_DIR      2

/* Extent map entry (per design doc).
 * em_size low 24 bits = size in 4K units.
 * em_size top 8 bits  = flags (bit0 = em_lba is in 128 MiB units, else 4 KB).
 */
struct __attribute__((packed)) nebula_extent_map_entry {
    uint64_t em_offset;          /* logical offset, 4 KB units */
    uint32_t em_size_and_flags;  /* 24b size | 8b flags */
    uint32_t em_lba;             /* physical LBA (4 KB or 128 MiB based on flags) */
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_extent_map_entry) == NEBULA_EXTENT_ENTRY_SIZE,
               "extent entry must be 16 B");

#define NEBULA_EM_FLAG_LBA_128MIB  0x01

struct __attribute__((packed)) nebula_inode {
    /* --- Attributes (128 bytes) --- */
    uint32_t magic;
    uint32_t version;
    uint64_t inode_num;
    uint32_t type;               /* FREE / FILE / DIR */
    uint32_t mode;
    uint64_t size_bytes;         /* logical size */
    uint64_t alloc_size_bytes;   /* actually allocated */
    uint64_t atime_ns;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
    uint32_t nlink;
    uint32_t flags;
    uint32_t checksum;
    uint8_t  attr_reserved[NEBULA_INODE_ATTR_SIZE - 76];

    /* --- Extent map (3968 bytes = 248 entries) --- */
    struct nebula_extent_map_entry extents[NEBULA_EXTENTS_PER_INODE];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_inode) == NEBULA_BLOCK_SIZE, "Inode must be 4KB");

/* ==================================================================
 * Directory page header - first block of directory region
 * ================================================================== */
struct __attribute__((packed)) nebula_dir_page_header {
    uint64_t magic;
    uint32_t version;
    uint32_t entry_size;          /* 1024 */
    uint64_t capacity;            /* max entries */
    uint64_t num_entries;         /* current count */
    uint32_t checksum;
    uint8_t  reserved[NEBULA_BLOCK_SIZE - 36];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_dir_page_header) == NEBULA_BLOCK_SIZE,
               "dir page header 4KB");

/* Directory entry - 1024 B; 4 per 4 KB block */
#define NEBULA_DIR_ENTRY_SIZE 1024U
#define NEBULA_DIR_NAME_MAX   1000U   /* 1024 - 24 header bytes */
#define NEBULA_DIR_ENTRIES_PER_BLOCK (NEBULA_BLOCK_SIZE / NEBULA_DIR_ENTRY_SIZE)

#define NEBULA_DIR_FLAG_USED   0x01
#define NEBULA_DIR_FLAG_FILE   0x02
#define NEBULA_DIR_FLAG_DIR    0x04

/* ==================================================================
 * Stream Record - 1 byte allocation-log entry (see design doc §9)
 *   op == NEBULA_STREAM_OP_FREE  (0) => block freed
 *   op == NEBULA_STREAM_OP_ALLOC (1) => block allocated
 *
 * Records are appended sequentially into the Stream Map region.
 * The LBA they apply to is derived from position (record N => block N
 * within the allocator root currently being logged).  A stream page
 * (4 KB) holds 4096 records.  The current write offset is tracked in
 * the active uberblock (`stream_map_head_offset`).
 * ================================================================== */
#define NEBULA_STREAM_OP_FREE  0
#define NEBULA_STREAM_OP_ALLOC 1

struct __attribute__((packed)) nebula_stream_record {
    uint8_t op;    /* NEBULA_STREAM_OP_{FREE,ALLOC} */
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_stream_record) == 1,
               "stream record must be exactly 1 byte");

#define NEBULA_STREAM_RECORDS_PER_PAGE (NEBULA_BLOCK_SIZE)  /* 4096 */

/* ==================================================================
 * Root-inode sub-allocator (design doc §11)
 *
 * The root inode extends its data region in 128 MiB chunks pulled from the
 * global block allocator.  Within each chunk, directory inodes are handed
 * out in 32 KiB units.  A 4 KiB bitmap lives at the top of every such
 * chunk, tracking the 32768 contained 4 KiB sub-blocks (one bit each).
 *
 *    128 MiB / 4 KiB = 32768 sub-blocks
 *    32768 bits      / 8    = 4096 bytes = 1 page
 *
 * An allocated 32 KiB directory slot consumes 8 consecutive bits.
 * ================================================================== */
#define NEBULA_ROOT_CHUNK_BYTES         NEBULA_CHUNK_SIZE        /* 128 MiB */
#define NEBULA_ROOT_CHUNK_SUBBLOCK_SIZE NEBULA_BLOCK_SIZE        /* 4 KiB   */
#define NEBULA_ROOT_CHUNK_SUBBLOCKS     NEBULA_CHUNK_BLOCKS      /* 32768   */
#define NEBULA_ROOT_DIR_SLOT_BYTES      (32U * 1024U)            /* 32 KiB  */
#define NEBULA_ROOT_DIR_SLOT_SUBBLOCKS  (NEBULA_ROOT_DIR_SLOT_BYTES / \
                                         NEBULA_ROOT_CHUNK_SUBBLOCK_SIZE) /* 8 */
#define NEBULA_ROOT_DIR_SLOTS_PER_CHUNK \
        (NEBULA_ROOT_CHUNK_BYTES / NEBULA_ROOT_DIR_SLOT_BYTES)   /* 4096 */

struct __attribute__((packed)) nebula_root_chunk_bitmap {
    uint8_t bits[NEBULA_BLOCK_SIZE];  /* 32768 bits, 1 per 4 KiB sub-block */
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_root_chunk_bitmap) == NEBULA_BLOCK_SIZE,
               "root-chunk bitmap must be 4 KiB");

struct __attribute__((packed)) nebula_dir_entry {
    uint64_t hash;
    uint64_t inode_num;
    uint16_t name_len;
    uint16_t flags;
    uint32_t _pad0;
    char     name[NEBULA_DIR_NAME_MAX];
};
NEBULA_STATIC_ASSERT(sizeof(struct nebula_dir_entry) == NEBULA_DIR_ENTRY_SIZE,
               "dir entry must be 1024 B");

#endif /* NEBULA_FORMAT_H */
