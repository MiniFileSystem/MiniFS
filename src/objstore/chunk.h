/*
 * objstore/chunk.h - Chunk lifecycle engine.
 *
 * A chunk is a 64 MiB contiguous LBA range allocated from the underlying
 * Nebula device via nebula_block_alloc().  Once allocated, the chunk is
 * append-only: each segment claims an offset, gets a chunk_idx, and the
 * chunk is sealed when it cannot accept the next append.  Only one
 * active chunk exists at a time.  Sealed chunks are immutable.
 *
 * The engine NEVER performs the actual data write; it only reserves
 * space and produces (chunk_id, chunk_idx, offset, length).  The
 * ObjectStore submits the write through nebula_io_write() after
 * reserving.  This separation lets the caller batch many appends, fire
 * all IOs, then wait once before committing metadata - the durability
 * barrier.
 *
 * Phase 1+2 simplifications:
 *   - All segments are padded up to NEBULA_BLOCK_SIZE (4 KiB).
 *   - Forward map (logical -> physical) is stored under  ex: keys.
 *   - Reverse map (physical -> logical) is stored under  ci: keys for
 *     GC; written by  stage_chunk_rev() inside the same TXG batch.
 */
#ifndef NEBULA_OBJSTORE_CHUNK_H
#define NEBULA_OBJSTORE_CHUNK_H

#include "common.h"
#include "metadata.h"

#include <mutex>
#include <optional>

extern "C" {
struct nebula_mount;
}

namespace nebula::objstore {

class ChunkEngine {
public:
    ChunkEngine(struct nebula_mount *mount, MetadataStore *metadata);

    /* On open, adopt an existing unsealed chunk if one exists. */
    Status open();

    struct Reservation {
        uint32_t chunk_id;
        uint16_t chunk_idx;
        uint32_t offset_bytes;
        uint32_t length_bytes;     /* actual reserved (may be < requested) */
        uint32_t padded_bytes;     /* round_up_block(length_bytes) */
        uint64_t physical_lba;
    };

    /* Reserve up to `max_length` bytes in the active chunk.  The actual
     * reserved length is min(max_length, space_left_in_active_chunk),
     * capped to kChunkSize.  If the active chunk is full (or absent),
     * the engine seals it and allocates a fresh 64 MiB chunk first.
     * The caller MUST loop on remaining bytes; one call per chunk
     * boundary is the common case. */
    Status reserve(uint32_t max_length, Reservation *out);

    /* Stage the reverse-map entry (ci:<chunk_id><idx> -> {oid, off, len})
     * inside the caller's TXG batch.  Done after the data write so the
     * batch becomes durable atomically with the forward-map insert. */
    static void stage_chunk_rev(MetadataStore::Batch &batch,
                                uint32_t chunk_id, uint16_t idx,
                                uint64_t oid, uint64_t logical_offset,
                                uint32_t length);

    /* Snapshot the active chunk record (write_offset / next_idx) into
     * the batch so its metadata moves atomically with the extent inserts. */
    void stage_active_chunk(MetadataStore::Batch &batch);

    /* Resolve chunk_id -> Chunk (lba_start needed by READ to compute
     * the physical LBA). */
    Status get_chunk(uint32_t chunk_id, Chunk *out);

private:
    Status seal_active_locked();
    Status allocate_new_chunk_locked();

    struct nebula_mount *mount_;
    MetadataStore       *meta_;

    std::mutex          mu_;
    std::optional<Chunk> active_;
};

} /* namespace nebula::objstore */

#endif /* NEBULA_OBJSTORE_CHUNK_H */
