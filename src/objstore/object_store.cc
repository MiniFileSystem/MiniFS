/* objstore/object_store.cc - CREATE / WRITE / READ implementation. */
#include "object_store.h"
#include "metadata.h"
#include "chunk.h"

extern "C" {
#include "nebula/nebula_fs.h"
#include "nebula/nebula_io.h"
#include "../nebula/nebula_mount.h"
#include "../util/crc32c.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nebula::objstore {

namespace {
constexpr uint32_t kBlock = kBlockSize;

inline void *alloc_aligned_block(size_t bytes)
{
    void *p = nullptr;
    if (posix_memalign(&p, kBlock, bytes) != 0) return nullptr;
    return p;
}

} /* anon */

ObjectStore::ObjectStore() = default;
ObjectStore::~ObjectStore() { close(); }

Status ObjectStore::open(const OpenOptions &opts)
{
    opts_ = opts;

    nebula_fs_t *fs = nullptr;
    int rc = nebula_fs_mount(opts.device_path.c_str(), &fs);
    if (rc != 0) {
        std::fprintf(stderr, "ObjectStore::open: nebula_fs_mount(%s) failed: %d\n",
                     opts.device_path.c_str(), rc);
        return Status::ERROR_IO_FAILURE;
    }
    mount_ = fs;

    meta_ = std::make_unique<MetadataStore>();
    Status s = meta_->open(opts.metadata_path);
    if (s != Status::OK) {
        nebula_fs_unmount(mount_);
        mount_ = nullptr;
        return s;
    }

    chunks_ = std::make_unique<ChunkEngine>(mount_, meta_.get());
    s = chunks_->open();
    if (s != Status::OK) {
        meta_->close();
        nebula_fs_unmount(mount_);
        mount_ = nullptr;
        return s;
    }
    return Status::OK;
}

void ObjectStore::close()
{
    chunks_.reset();
    if (meta_) meta_->close();
    meta_.reset();
    if (mount_) nebula_fs_unmount(mount_);
    mount_ = nullptr;
}

/* ---- block I/O helpers ----------------------------------------------- */
Status ObjectStore::write_blocks(uint64_t physical_lba, const void *buf,
                                 uint32_t n_blocks)
{
    int rc = nebula_io_write(mount_->io,
                             static_cast<nebula_lba_t>(physical_lba),
                             n_blocks, buf);
    return rc == 0 ? Status::OK : Status::ERROR_IO_FAILURE;
}

Status ObjectStore::read_blocks(uint64_t physical_lba, void *buf,
                                uint32_t n_blocks)
{
    int rc = nebula_io_read(mount_->io,
                            static_cast<nebula_lba_t>(physical_lba),
                            n_blocks, buf);
    return rc == 0 ? Status::OK : Status::ERROR_IO_FAILURE;
}

/* ----------------------------------------------------------------------
 * CREATE
 *
 * Validates path, allocates oid, persists ph: + at: atomically.  Zero
 * device I/O.  The extent map is empty and stays empty until WRITE.
 * ---------------------------------------------------------------------- */
Status ObjectStore::create(const std::string &path)
{
    if (!validate_object_path(path))
        return Status::ERROR_INVALID_ARGUMENT;

    if (meta_->path_exists(path))
        return Status::ERROR_ALREADY_EXISTS;

    uint64_t oid = 0;
    Status s = meta_->allocate_object_id(&oid);
    if (s != Status::OK) return s;

    ObjectAttrs a{};
    a.object_id   = oid;
    a.size        = 0;
    a.ctime_ns    = now_ns();
    a.mtime_ns    = a.ctime_ns;
    a.version     = 1;
    a.num_extents = 0;

    MetadataStore::Batch batch;
    batch.put_attrs(a);
    batch.put_path(path, oid);

    return meta_->commit(batch, /*sync=*/opts_.sync_metadata);
}

/* ----------------------------------------------------------------------
 * WRITE
 *
 * Append-only.  For each kAppendSize-sized segment:
 *   1. Reserve space in the active chunk.
 *   2. Sync block write (durability barrier).
 *   3. Stage forward map  ex:<oid><logical_off> -> ExtentEntry
 *      and reverse map  ci:<chunk_id><idx>      -> ChunkRevEntry .
 * After the loop, stage updated attrs + active chunk record, commit.
 * ---------------------------------------------------------------------- */
Status ObjectStore::write(const std::string &path, const void *data, size_t len)
{
    if (!validate_object_path(path)) return Status::ERROR_INVALID_ARGUMENT;
    if (!data && len > 0)            return Status::ERROR_INVALID_ARGUMENT;

    uint64_t oid = 0;
    Status s = meta_->path_lookup(path, &oid);
    if (s != Status::OK) return s;

    ObjectAttrs a{};
    s = meta_->get_attrs(oid, &a);
    if (s != Status::OK) return s;

    if (len == 0) return Status::OK;
    if (len > UINT32_MAX) {
        /* The extent map's length field is uint32_t (4 GiB max per
         * extent).  Larger writes would still work because reserve()
         * caps each extent at kChunkSize=64 MiB, but we'd have to
         * relax this loop's accounting; for now keep the contract
         * simple: caller chunks anything > 4 GiB themselves. */
        return Status::ERROR_INVALID_ARGUMENT;
    }

    MetadataStore::Batch batch;

    const uint8_t *src = static_cast<const uint8_t *>(data);
    size_t   remaining = len;
    size_t   cursor    = 0;
    uint64_t logical   = a.size;

    /* Single 4 KiB scratch buffer used ONLY for the trailing partial
     * block of an extent whose length is not block-aligned (i.e. the
     * very last segment of a write whose total size is not a multiple
     * of 4 KiB).  All other I/O issues pwrite directly from the
     * caller's buffer - no extra memcpy, no extra memset. */
    void *tail_scratch = alloc_aligned_block(kBlock);
    if (!tail_scratch) return Status::ERROR_INTERNAL;

    Status final_status = Status::OK;
    uint64_t added_extents = 0;
    while (remaining > 0) {
        /* Reserve as much chunk space as we can use this iteration.
         * Capped at 4 GiB to fit ExtentEntry::length (uint32_t). */
        uint32_t want = static_cast<uint32_t>(
            std::min<size_t>(remaining, static_cast<size_t>(UINT32_MAX)));

        ChunkEngine::Reservation r{};
        s = chunks_->reserve(want, &r);
        if (s != Status::OK) { final_status = s; break; }

        /* Step 1: data path - synchronous so completion == durable.
         *
         * Split the extent into:
         *   bulk_blocks   : aligned 4 KiB-multiple portion (most bytes)
         *   tail_bytes    : 0..kBlock-1 trailing bytes that need a
         *                   zero-padded final block
         *
         * Most segments are bulk_only because chunk space is always
         * block-aligned; only the very last segment of a write whose
         * total length is not a multiple of 4 KiB has a tail. */
        uint32_t bulk_bytes = r.length_bytes & ~(kBlock - 1);
        uint32_t tail_bytes = r.length_bytes - bulk_bytes;

        if (bulk_bytes > 0) {
            /* pwrite directly from user buffer - no copy, no padding. */
            s = write_blocks(r.physical_lba,
                             src + cursor,
                             bulk_bytes / kBlock);
            if (s != Status::OK) { final_status = s; break; }
        }
        if (tail_bytes > 0) {
            std::memset(tail_scratch, 0, kBlock);
            std::memcpy(tail_scratch, src + cursor + bulk_bytes, tail_bytes);
            s = write_blocks(r.physical_lba + bulk_bytes / kBlock,
                             tail_scratch, 1);
            if (s != Status::OK) { final_status = s; break; }
        }

        /* Step 2: stage the forward map (logical -> physical).
         * Checksum is left zero - read path doesn't verify today;
         * re-add when read-side verification lands. */
        ExtentEntry e{};
        e.chunk_id        = r.chunk_id;
        e.chunk_idx       = r.chunk_idx;
        e.reserved        = 0;
        e.offset_in_chunk = r.offset_bytes;
        e.length          = r.length_bytes;
        e.checksum        = 0;
        batch.put_extent(oid, logical, e);

        /* Step 3: stage the reverse map (physical -> logical, for GC). */
        ChunkEngine::stage_chunk_rev(batch, r.chunk_id, r.chunk_idx,
                                      oid, logical, r.length_bytes);

        logical    += r.length_bytes;
        remaining  -= r.length_bytes;
        cursor     += r.length_bytes;
        added_extents += 1;
    }

    std::free(tail_scratch);

    if (final_status != Status::OK) return final_status;

    /* Step 3: durability barrier.  POSIX nebula_io_write is pwrite-only;
     * the data sits in the OS page cache until fsync.  SPDK NVMe writes
     * complete only after the device ack, so this is a no-op there.
     * Either way, after this call returns the segment payloads are
     * durable BEFORE we commit any metadata that references them. */
    if (nebula_io_flush(mount_->io) != 0)
        return Status::ERROR_IO_FAILURE;

    /* Step 4: prepare attrs. */
    a.size        += len;
    a.version     += 1;
    a.mtime_ns    = now_ns();
    a.num_extents += added_extents;
    batch.put_attrs(a);

    /* Stage updated active chunk record (write_offset / next_idx moved). */
    chunks_->stage_active_chunk(batch);

    return meta_->commit(batch, /*sync=*/opts_.sync_metadata);
}

/* ----------------------------------------------------------------------
 * READ
 *
 * Iterator-based prefix scan over ex:<be64 oid>.  We seek to the
 * extent that covers `offset` (using SeekForPrev) and walk forward
 * until we have served `size` bytes or reached the end of the range.
 *
 * For each extent:
 *   physical_lba = chunk.lba_start + extent.offset_in_chunk / 4096
 *   nebula_io_read(physical_lba, padded(extent.length))
 *   copy [in_extent_off, in_extent_off + to_copy) into the output
 *
 * Chunks are cached in a local map for the duration of the call so a
 * 1280-extent read of an 80 MiB object pays only 2 ck:* lookups (one
 * per chunk), not 1280.
 * ---------------------------------------------------------------------- */
Status ObjectStore::read(const std::string &path, uint64_t offset,
                         size_t size, std::vector<uint8_t> *out)
{
    if (!out) return Status::ERROR_INVALID_ARGUMENT;
    out->clear();

    /* Probe the actual readable size first, so we resize once to the
     * exact final length (no over-allocation, no second pass). */
    if (!validate_object_path(path)) return Status::ERROR_INVALID_ARGUMENT;
    uint64_t oid = 0;
    Status s = meta_->path_lookup(path, &oid);
    if (s != Status::OK) return s;
    ObjectAttrs a{};
    s = meta_->get_attrs(oid, &a);
    if (s != Status::OK) return s;
    if (offset > a.size) return Status::ERROR_OUT_OF_RANGE;
    uint64_t avail = a.size - offset;
    if (size > avail) size = static_cast<size_t>(avail);
    if (size == 0) return Status::OK;

    /* NOTE: resize() value-initializes (zero-fills) the new bytes,
     * which on first allocation costs one full memory-bandwidth pass
     * AND triggers anonymous-page faults page-by-page.  For large
     * reads this can dominate runtime (e.g. 64 MiB resize ~= 16k page
     * faults).  Hot paths should use the raw-buffer overload which
     * lets the caller reuse a pre-warmed buffer. */
    out->resize(size);
    size_t got = 0;
    s = read(path, offset, size, out->data(), &got);
    if (s != Status::OK) {
        out->clear();
        return s;
    }
    if (got != size) out->resize(got);
    return Status::OK;
}

Status ObjectStore::read(const std::string &path, uint64_t offset,
                         size_t dst_cap, void *dst_buf, size_t *out_bytes)
{
    if (!out_bytes)                      return Status::ERROR_INVALID_ARGUMENT;
    if (!dst_buf && dst_cap > 0)         return Status::ERROR_INVALID_ARGUMENT;
    if (!validate_object_path(path))     return Status::ERROR_INVALID_ARGUMENT;
    *out_bytes = 0;

    uint64_t oid = 0;
    Status s = meta_->path_lookup(path, &oid);
    if (s != Status::OK) return s;

    ObjectAttrs a{};
    s = meta_->get_attrs(oid, &a);
    if (s != Status::OK) return s;

    if (offset > a.size) return Status::ERROR_OUT_OF_RANGE;
    uint64_t avail = a.size - offset;
    size_t   size  = (dst_cap > avail) ? static_cast<size_t>(avail) : dst_cap;
    if (size == 0) return Status::OK;

    uint8_t *dst = static_cast<uint8_t *>(dst_buf);

    /* Small per-call scratch used ONLY for misaligned head/tail
     * blocks at extent boundaries (rare: head=0 unless caller asked
     * for an unaligned offset; tail=0 unless extent length is not a
     * 4 KiB multiple, which only happens at object EOF). */
    void *scratch = alloc_aligned_block(kBlock);
    if (!scratch) return Status::ERROR_INTERNAL;

    /* Per-call chunk lookup cache. */
    std::unordered_map<uint32_t, Chunk> chunk_cache;

    Status visit_status = Status::OK;
    uint64_t cursor_offset = offset;       /* logical bytes still to skip */
    size_t   remaining     = size;

    auto visitor = [&](uint64_t ext_off, const ExtentEntry &e) -> bool {
        if (remaining == 0) return false;

        uint64_t ext_end = ext_off + e.length;
        if (ext_end <= cursor_offset) return true;   /* shouldn't happen */
        if (ext_off >= cursor_offset + remaining) return false;

        /* Resolve chunk (cached). */
        auto it = chunk_cache.find(e.chunk_id);
        if (it == chunk_cache.end()) {
            Chunk c{};
            Status cs = chunks_->get_chunk(e.chunk_id, &c);
            if (cs != Status::OK) { visit_status = cs; return false; }
            it = chunk_cache.emplace(e.chunk_id, c).first;
        }
        const Chunk &c = it->second;

        size_t in_ext_off = (cursor_offset > ext_off)
                              ? static_cast<size_t>(cursor_offset - ext_off)
                              : 0;
        size_t in_ext_left = static_cast<size_t>(e.length) - in_ext_off;
        size_t to_copy     = std::min<size_t>(remaining, in_ext_left);

        /* Three-phase per-extent read:
         *   (a) head:  if phys_byte_off is not block-aligned, pull in
         *              one block via scratch and copy out the
         *              relevant suffix.
         *   (b) bulk:  pread N full blocks DIRECTLY into dst - this is
         *              the only path most bytes take.
         *   (c) tail:  trailing 0..kBlock-1 bytes (if to_copy didn't
         *              end on a block boundary) via scratch.
         * For a 64 MiB chunk-bounded extent aligned at offset 0 this
         * is exactly one pread of (16384 blocks) directly into dst -
         * one syscall, one DMA path, zero memcpy. */
        uint64_t phys_byte_off = e.offset_in_chunk + in_ext_off;

        /* (a) misaligned head */
        uint32_t head_skip = static_cast<uint32_t>(phys_byte_off & (kBlock - 1));
        if (head_skip > 0 && to_copy > 0) {
            uint64_t lba = c.lba_start + (phys_byte_off / kBlock);
            Status rs = read_blocks(lba, scratch, 1);
            if (rs != Status::OK) { visit_status = rs; return false; }

            size_t avail_in_block = kBlock - head_skip;
            size_t this_copy = std::min<size_t>(to_copy, avail_in_block);
            std::memcpy(dst + (size - remaining),
                        static_cast<const uint8_t *>(scratch) + head_skip,
                        this_copy);
            phys_byte_off += this_copy;
            to_copy       -= this_copy;
            cursor_offset += this_copy;
            remaining     -= this_copy;
        }

        /* (b) aligned bulk - direct pread into dst, no copy */
        if (to_copy >= kBlock) {
            uint32_t bulk_blocks = static_cast<uint32_t>(to_copy / kBlock);
            size_t   bulk_bytes  = static_cast<size_t>(bulk_blocks) * kBlock;
            uint64_t lba = c.lba_start + (phys_byte_off / kBlock);
            Status rs = read_blocks(lba, dst + (size - remaining), bulk_blocks);
            if (rs != Status::OK) { visit_status = rs; return false; }
            phys_byte_off += bulk_bytes;
            to_copy       -= bulk_bytes;
            cursor_offset += bulk_bytes;
            remaining     -= bulk_bytes;
        }

        /* (c) misaligned tail */
        if (to_copy > 0) {
            uint64_t lba = c.lba_start + (phys_byte_off / kBlock);
            Status rs = read_blocks(lba, scratch, 1);
            if (rs != Status::OK) { visit_status = rs; return false; }
            std::memcpy(dst + (size - remaining), scratch, to_copy);
            phys_byte_off += to_copy;
            cursor_offset += to_copy;
            remaining     -= to_copy;
            to_copy        = 0;
        }

        return remaining > 0;
    };

    s = meta_->iter_extents(oid, offset, offset + size, visitor);
    std::free(scratch);
    if (visit_status != Status::OK) return visit_status;
    if (s != Status::OK) return s;
    *out_bytes = size - remaining;
    return Status::OK;
}

Status ObjectStore::stat(const std::string &path, ObjectAttrs *out)
{
    if (!out)                         return Status::ERROR_INVALID_ARGUMENT;
    if (!validate_object_path(path))  return Status::ERROR_INVALID_ARGUMENT;
    uint64_t oid = 0;
    Status s = meta_->path_lookup(path, &oid);
    if (s != Status::OK) return s;
    return meta_->get_attrs(oid, out);
}

Status ObjectStore::chunk_id_range(const std::string &path,
                                    uint32_t *out_min, uint32_t *out_max)
{
    if (!out_min || !out_max)         return Status::ERROR_INVALID_ARGUMENT;
    if (!validate_object_path(path))  return Status::ERROR_INVALID_ARGUMENT;

    uint64_t oid = 0;
    Status s = meta_->path_lookup(path, &oid);
    if (s != Status::OK) return s;

    ObjectAttrs a{};
    s = meta_->get_attrs(oid, &a);
    if (s != Status::OK) return s;

    if (a.size == 0) {
        *out_min = kInvalidChunkId;
        *out_max = kInvalidChunkId;
        return Status::OK;
    }

    uint32_t lo = UINT32_MAX, hi = 0;
    auto visit = [&](uint64_t /*off*/, const ExtentEntry &e) -> bool {
        if (e.chunk_id < lo) lo = e.chunk_id;
        if (e.chunk_id > hi) hi = e.chunk_id;
        return true;
    };
    s = meta_->iter_extents(oid, 0, a.size, visit);
    if (s != Status::OK) return s;
    *out_min = lo;
    *out_max = hi;
    return Status::OK;
}

} /* namespace nebula::objstore */
