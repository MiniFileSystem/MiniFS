/*
 * objstore/metadata.h - RocksDB-backed metadata store (extent-keyed).
 *
 * Owns ALL persistent metadata for the object store:
 *   - path -> object_id            (ph:<path>)
 *   - object_id -> ObjectAttrs     (at:<be64 oid>)
 *   - per-extent forward map       (ex:<be64 oid><be64 logical_offset>)
 *   - chunk records                (ck:<be32 chunk_id>)
 *   - chunk reverse map for GC     (ci:<be32 chunk_id><be16 idx>)
 *   - global counters              (ct:next_object_id, ct:next_chunk_id,
 *                                   ct:schema_version)
 *
 * No data bytes pass through this module.
 */
#ifndef NEBULA_OBJSTORE_METADATA_H
#define NEBULA_OBJSTORE_METADATA_H

#include "common.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace rocksdb {
    class DB;
    class Iterator;
    class WriteBatch;
}

namespace nebula::objstore {

class MetadataStore {
public:
    MetadataStore();
    ~MetadataStore();

    /* Open or create a RocksDB instance at `path`.  Refuses to open
     * databases with a different schema version.  Initializes counters
     * on first run. */
    Status open(const std::string &path);
    void   close();

    /* Counters (atomic, persistent). */
    Status allocate_object_id(uint64_t *out);
    Status allocate_chunk_id(uint32_t *out);

    /* Path <-> object id. */
    bool   path_exists(const std::string &path);
    Status path_lookup(const std::string &path, uint64_t *out_oid);

    /* Attributes. */
    Status get_attrs(uint64_t oid, ObjectAttrs *out);

    /* Chunks. */
    Status get_chunk(uint32_t chunk_id, Chunk *out);
    Status put_chunk(const Chunk &c);

    /* Extent map iteration:
     *   visit every extent whose `logical_offset` is in
     *   [start_offset_inclusive, end_offset_exclusive), in ascending
     *   order, calling `cb(logical_offset, extent)`.  If the range
     *   straddles an extent that starts BEFORE start_offset, that
     *   extent is included as the first one.
     *
     *   Stops early if cb returns false.
     */
    using ExtentVisitor =
        std::function<bool(uint64_t logical_offset, const ExtentEntry &)>;
    Status iter_extents(uint64_t oid,
                        uint64_t start_offset_inclusive,
                        uint64_t end_offset_exclusive,
                        const ExtentVisitor &cb);

    /* ---- Batched writes (used by TXG commit) ------------------------- */
    class Batch {
    public:
        explicit Batch();
        ~Batch();

        void put_path  (const std::string &path, uint64_t oid);
        void put_attrs (const ObjectAttrs &a);
        void put_extent(uint64_t oid, uint64_t logical_offset,
                        const ExtentEntry &e);
        void put_chunk (const Chunk &c);
        void put_chunk_rev(uint32_t chunk_id, uint16_t idx,
                           const ChunkRevEntry &r);

    private:
        friend class MetadataStore;
        std::unique_ptr<rocksdb::WriteBatch> wb_;
    };

    Status commit(Batch &batch, bool sync);

    rocksdb::DB *raw() { return db_; }

private:
    MetadataStore(const MetadataStore &) = delete;
    MetadataStore &operator=(const MetadataStore &) = delete;

    Status init_or_check_schema_locked();
    Status read_counters_locked();
    Status persist_counters_locked();

    rocksdb::DB *db_ = nullptr;
    std::mutex   mu_;
    uint64_t     next_object_id_ = 1;
    uint32_t     next_chunk_id_  = 1;
};

} /* namespace nebula::objstore */

#endif /* NEBULA_OBJSTORE_METADATA_H */
