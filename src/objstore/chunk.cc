/* objstore/chunk.cc - Chunk lifecycle engine. */
#include "chunk.h"

extern "C" {
#include "nebula/nebula_types.h"
#include "../nebula/nebula_block_alloc.h"
}

#include <algorithm>
#include <cstdio>

namespace nebula::objstore {

namespace {
constexpr uint32_t kBlock = kBlockSize;

inline uint32_t round_up_block(uint32_t bytes)
{
    return (bytes + kBlock - 1) & ~(kBlock - 1);
}
} /* anon */

ChunkEngine::ChunkEngine(struct nebula_mount *mount, MetadataStore *metadata)
    : mount_(mount), meta_(metadata) {}

Status ChunkEngine::open()
{
    /* Forward scan from chunk_id 1 until NotFound; adopt any unsealed
     * chunk we encounter (there should be at most one). */
    std::lock_guard<std::mutex> g(mu_);
    active_.reset();
    for (uint32_t id = 1; ; id++) {
        Chunk c;
        Status s = meta_->get_chunk(id, &c);
        if (s == Status::ERROR_NOT_FOUND) break;
        if (s != Status::OK) return s;
        if (!c.sealed) active_ = c;
    }
    return Status::OK;
}

Status ChunkEngine::allocate_new_chunk_locked()
{
    nebula_lba_t lba = 0;
    int rc = nebula_block_alloc(mount_, kChunkBlocks, &lba);
    if (rc != 0) {
        std::fprintf(stderr, "ChunkEngine: nebula_block_alloc(%u) failed: %d\n",
                     kChunkBlocks, rc);
        return Status::ERROR_NO_SPACE;
    }

    uint32_t cid = 0;
    Status s = meta_->allocate_chunk_id(&cid);
    if (s != Status::OK) return s;

    Chunk c{};
    c.chunk_id     = cid;
    c.lba_start    = static_cast<uint64_t>(lba);
    c.write_offset = 0;
    c.next_idx     = 0;
    c.sealed       = false;

    s = meta_->put_chunk(c);
    if (s != Status::OK) return s;

    active_ = c;
    return Status::OK;
}

Status ChunkEngine::seal_active_locked()
{
    if (!active_) return Status::OK;
    active_->sealed = true;
    Status s = meta_->put_chunk(*active_);
    active_.reset();
    return s;
}

Status ChunkEngine::reserve(uint32_t max_length, Reservation *out)
{
    if (max_length == 0) return Status::ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> g(mu_);

    /* Ensure we have an active chunk with at least one block of room.
     * Note: write_offset is always a multiple of kBlock by construction
     * (every reservation advances by `padded_bytes`), so checking
     * `write_offset >= kChunkSize` is equivalent to "no room left". */
    if (!active_ || active_->write_offset >= kChunkSize) {
        if (active_) {
            Status s = seal_active_locked();
            if (s != Status::OK) return s;
        }
        Status s = allocate_new_chunk_locked();
        if (s != Status::OK) return s;
    }

    Chunk &c = *active_;
    uint32_t space_left = kChunkSize - c.write_offset;

    /* Clamp the request to whatever fits in the active chunk.  The
     * caller loops on remaining bytes; the next iteration will hit the
     * full-chunk branch above and allocate a fresh chunk. */
    uint32_t actual = std::min(max_length, space_left);
    uint32_t padded = round_up_block(actual);
    /* Padding can push us slightly past chunk capacity if `actual` is
     * not block-aligned AND space_left is not block-aligned.  Since
     * write_offset is always block-aligned, space_left is too, so this
     * never fires in practice; clamp defensively anyway. */
    if (padded > space_left) {
        padded = space_left;
        actual = padded;
    }

    out->chunk_id     = c.chunk_id;
    out->chunk_idx    = static_cast<uint16_t>(c.next_idx);
    out->offset_bytes = c.write_offset;
    out->length_bytes = actual;
    out->padded_bytes = padded;
    out->physical_lba = c.lba_start + (c.write_offset / kBlock);

    c.write_offset += padded;
    c.next_idx     += 1;
    return Status::OK;
}

void ChunkEngine::stage_chunk_rev(MetadataStore::Batch &batch,
                                   uint32_t chunk_id, uint16_t idx,
                                   uint64_t oid, uint64_t logical_offset,
                                   uint32_t length)
{
    ChunkRevEntry r{};
    r.oid            = oid;
    r.logical_offset = logical_offset;
    r.length         = length;
    r.reserved       = 0;
    batch.put_chunk_rev(chunk_id, idx, r);
}

void ChunkEngine::stage_active_chunk(MetadataStore::Batch &batch)
{
    std::lock_guard<std::mutex> g(mu_);
    if (active_) batch.put_chunk(*active_);
}

Status ChunkEngine::get_chunk(uint32_t chunk_id, Chunk *out)
{
    return meta_->get_chunk(chunk_id, out);
}

} /* namespace nebula::objstore */
