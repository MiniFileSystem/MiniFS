/*
 * objstore/common.h - Core data structures for the multi-tenant object store.
 *
 * Schema (version 2 - extent-keyed):
 *   ph:<path>                       -> uint64 oid
 *   at:<be64 oid>                   -> ObjectAttrs
 *   ex:<be64 oid><be64 logical_off> -> ExtentEntry      (forward map)
 *   ck:<be32 chunk_id>              -> Chunk
 *   ci:<be32 chunk_id><be16 idx>    -> ChunkRevEntry    (reverse map for GC)
 *   ct:next_object_id               -> uint64
 *   ct:next_chunk_id                -> uint32
 *
 * Big-endian encoding of numeric key components is REQUIRED so that
 * RocksDB's lexicographic key order matches numeric order; this is what
 * lets us do `Iterator::Seek` / `SeekForPrev` over the extent map and
 * iterate exactly the extents that cover a logical byte range.
 *
 * Path layout (strict, validated at every public entry):
 *   /<tenant>/<subtenant>/<dataset>/<objectid>
 *   - exactly four non-empty components
 *   - no embedded "/" inside a component
 */
#ifndef NEBULA_OBJSTORE_COMMON_H
#define NEBULA_OBJSTORE_COMMON_H

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "nebula/nebula_types.h"
}

namespace nebula::objstore {

/* Status / error codes returned by the public API. */
enum class Status {
    OK = 0,
    ERROR_NOT_FOUND,
    ERROR_ALREADY_EXISTS,
    ERROR_PARENT_NOT_FOUND,
    ERROR_OUT_OF_RANGE,
    ERROR_NO_SPACE,
    ERROR_IO_FAILURE,
    ERROR_METADATA,
    ERROR_INVALID_ARGUMENT,
    ERROR_INTERNAL,
};

const char *status_str(Status s);

/* ---- Tunables (compile-time for now) ---------------------------------- */
constexpr uint32_t kBlockSize        = 4096;            /* nebula block */
constexpr uint32_t kChunkSize        = 64u * 1024u * 1024u;  /* 64 MiB */
constexpr uint32_t kChunkBlocks      = kChunkSize / kBlockSize; /* 16384 */
constexpr uint32_t kAppendSize       = 64u * 1024u;     /* 64 KiB segments */
constexpr uint64_t kInvalidObjectId  = 0;
constexpr uint32_t kInvalidChunkId   = 0;

/* Schema version stored at "ct:schema_version".  Refuse to open older
 * databases.  Bump when on-disk encoding changes. */
constexpr uint32_t kSchemaVersion    = 2;

/* ---- Path validation -------------------------------------------------- */
/* Returns true iff `p` matches /<tenant>/<sub>/<ds>/<oid> exactly. */
bool validate_object_path(const std::string &p);

/* ----------------------------------------------------------------------
 * ObjectAttrs - the attribute portion of an inode.  No pointer list:
 * the extent map lives under  ex:<oid>:* .
 *
 * Stored at  at:<be64 oid> .
 * ---------------------------------------------------------------------- */
struct ObjectAttrs {
    uint64_t object_id;
    uint64_t size;
    uint64_t ctime_ns;
    uint64_t mtime_ns;
    uint64_t version;
    uint64_t num_extents;   /* maintained on write/truncate; cheap stat */
};

/* ----------------------------------------------------------------------
 * ExtentEntry - the forward map.  One key per append segment.
 *
 * Key:    ex:<be64 oid><be64 logical_offset>
 * Value:  ExtentEntry
 *
 * `chunk_id`  + `offset_in_chunk` give the physical location.
 * `chunk_idx` is kept so the reverse map (ci:) can be looked up for GC.
 * The READ path needs ONLY this struct + Chunk.lba_start to issue I/O.
 * ---------------------------------------------------------------------- */
struct ExtentEntry {
    uint32_t chunk_id;
    uint16_t chunk_idx;
    uint16_t reserved;          /* padding for clean serialization */
    uint32_t offset_in_chunk;   /* byte offset (block-aligned) */
    uint32_t length;            /* logical caller-visible byte count */
    uint64_t checksum;          /* crc32c over the padded write */
};
static_assert(sizeof(ExtentEntry) == 24, "ExtentEntry wire layout");

/* ----------------------------------------------------------------------
 * Chunk - an open or sealed 64 MiB region of the underlying device.
 *
 * Stored at  ck:<be32 chunk_id> .
 * ---------------------------------------------------------------------- */
struct Chunk {
    uint32_t chunk_id;
    uint64_t lba_start;
    uint32_t write_offset;
    uint32_t next_idx;
    bool     sealed;
};

/* ----------------------------------------------------------------------
 * ChunkRevEntry - the reverse map (physical -> logical) for GC.
 *
 * Stored at  ci:<be32 chunk_id><be16 idx> .
 * Given any physical (chunk_id, idx) GC can find the owning extent in
 * O(1) without scanning the entire ex: keyspace.  When that extent is
 * later overwritten / truncated, the corresponding ci: key becomes
 * orphaned and is what GC reclaims.
 * ---------------------------------------------------------------------- */
struct ChunkRevEntry {
    uint64_t oid;
    uint64_t logical_offset;
    uint32_t length;
    uint32_t reserved;
};
static_assert(sizeof(ChunkRevEntry) == 24, "ChunkRevEntry wire layout");

/* ---- Big-endian helpers ---------------------------------------------- */
void put_be16(std::string &out, uint16_t v);
void put_be32(std::string &out, uint32_t v);
void put_be64(std::string &out, uint64_t v);
uint16_t get_be16(const char *p);
uint32_t get_be32(const char *p);
uint64_t get_be64(const char *p);

/* ---- Serialization helpers ------------------------------------------- */
std::string encode_attrs(const ObjectAttrs &a);
bool        decode_attrs(const std::string &buf, ObjectAttrs *out);

std::string encode_extent(const ExtentEntry &e);
bool        decode_extent(const std::string &buf, ExtentEntry *out);

std::string encode_chunk(const Chunk &c);
bool        decode_chunk(const std::string &buf, Chunk *out);

std::string encode_chunk_rev(const ChunkRevEntry &e);
bool        decode_chunk_rev(const std::string &buf, ChunkRevEntry *out);

/* Wall-clock helpers */
uint64_t now_ns();

} /* namespace nebula::objstore */

#endif /* NEBULA_OBJSTORE_COMMON_H */
