/* objstore/metadata.cc - RocksDB-backed metadata store (extent-keyed). */
#include "metadata.h"

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <cstdio>
#include <cstring>

namespace nebula::objstore {

namespace {

/* Key encoders.  Numeric components are encoded BIG-ENDIAN so RocksDB's
 * lexicographic order matches numeric order; this is critical for
 * iter_extents() to work correctly via Seek / SeekForPrev. */

inline std::string key_path(const std::string &p) { return "ph:" + p; }

inline std::string key_attrs(uint64_t oid)
{
    std::string k = "at:";
    put_be64(k, oid);
    return k;
}

inline std::string key_extent_prefix(uint64_t oid)
{
    std::string k = "ex:";
    put_be64(k, oid);
    return k;          /* 11-byte prefix: 3 ASCII + 8 BE oid */
}

inline std::string key_extent(uint64_t oid, uint64_t logical_offset)
{
    std::string k = key_extent_prefix(oid);
    put_be64(k, logical_offset);
    return k;          /* 19 bytes: 3 + 8 + 8 */
}

inline std::string key_chunk(uint32_t cid)
{
    std::string k = "ck:";
    put_be32(k, cid);
    return k;
}

inline std::string key_chunk_rev(uint32_t cid, uint16_t idx)
{
    std::string k = "ci:";
    put_be32(k, cid);
    put_be16(k, idx);
    return k;
}

constexpr const char *kKeyNextObj   = "ct:next_object_id";
constexpr const char *kKeyNextChunk = "ct:next_chunk_id";
constexpr const char *kKeySchema    = "ct:schema_version";
constexpr const char *kKeyRootDir   = "ph:/";

template <typename T>
std::string pod_to_string(const T &v)
{
    return std::string(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
bool string_to_pod(const std::string &s, T *v)
{
    if (s.size() != sizeof(T)) return false;
    std::memcpy(v, s.data(), sizeof(T));
    return true;
}

} /* anon namespace */

MetadataStore::MetadataStore() = default;
MetadataStore::~MetadataStore() { close(); }

Status MetadataStore::open(const std::string &path)
{
    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.IncreaseParallelism(2);
    opts.OptimizeLevelStyleCompaction();
    rocksdb::Status rs = rocksdb::DB::Open(opts, path, &db_);
    if (!rs.ok()) {
        std::fprintf(stderr, "MetadataStore::open: %s\n", rs.ToString().c_str());
        return Status::ERROR_METADATA;
    }

    /* Root path entry so path_exists("/") is always true (used by future
     * directory-style helpers; today the strict 4-component validator
     * makes this purely informational). */
    std::string val;
    rs = db_->Get(rocksdb::ReadOptions(), kKeyRootDir, &val);
    if (rs.IsNotFound()) {
        uint64_t zero = 0;
        rs = db_->Put(rocksdb::WriteOptions(),
                      kKeyRootDir, pod_to_string(zero));
        if (!rs.ok()) return Status::ERROR_METADATA;
    } else if (!rs.ok()) {
        return Status::ERROR_METADATA;
    }

    Status s = init_or_check_schema_locked();
    if (s != Status::OK) return s;
    return read_counters_locked();
}

void MetadataStore::close()
{
    if (db_) { delete db_; db_ = nullptr; }
}

Status MetadataStore::init_or_check_schema_locked()
{
    std::string val;
    auto rs = db_->Get(rocksdb::ReadOptions(), kKeySchema, &val);
    if (rs.IsNotFound()) {
        rocksdb::WriteOptions wo;
        wo.sync = true;
        uint32_t v = kSchemaVersion;
        rs = db_->Put(wo, kKeySchema, pod_to_string(v));
        return rs.ok() ? Status::OK : Status::ERROR_METADATA;
    }
    if (!rs.ok()) return Status::ERROR_METADATA;
    uint32_t v = 0;
    if (!string_to_pod(val, &v)) return Status::ERROR_METADATA;
    if (v != kSchemaVersion) {
        std::fprintf(stderr,
            "MetadataStore: schema version mismatch (db=%u, code=%u). "
            "Delete the metadata directory to start fresh.\n",
            v, kSchemaVersion);
        return Status::ERROR_METADATA;
    }
    return Status::OK;
}

Status MetadataStore::read_counters_locked()
{
    std::string val;

    auto rs = db_->Get(rocksdb::ReadOptions(), kKeyNextObj, &val);
    if (rs.IsNotFound()) {
        next_object_id_ = 1;
    } else if (rs.ok()) {
        if (!string_to_pod(val, &next_object_id_)) return Status::ERROR_METADATA;
    } else {
        return Status::ERROR_METADATA;
    }

    rs = db_->Get(rocksdb::ReadOptions(), kKeyNextChunk, &val);
    if (rs.IsNotFound()) {
        next_chunk_id_ = 1;
    } else if (rs.ok()) {
        if (!string_to_pod(val, &next_chunk_id_)) return Status::ERROR_METADATA;
    } else {
        return Status::ERROR_METADATA;
    }
    return persist_counters_locked();
}

Status MetadataStore::persist_counters_locked()
{
    rocksdb::WriteBatch wb;
    wb.Put(kKeyNextObj,   pod_to_string(next_object_id_));
    wb.Put(kKeyNextChunk, pod_to_string(next_chunk_id_));
    rocksdb::WriteOptions wo;
    wo.sync = true;
    auto rs = db_->Write(wo, &wb);
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

Status MetadataStore::allocate_object_id(uint64_t *out)
{
    std::lock_guard<std::mutex> g(mu_);
    *out = next_object_id_++;
    rocksdb::WriteOptions wo;
    wo.sync = true;
    auto rs = db_->Put(wo, kKeyNextObj, pod_to_string(next_object_id_));
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

Status MetadataStore::allocate_chunk_id(uint32_t *out)
{
    std::lock_guard<std::mutex> g(mu_);
    *out = next_chunk_id_++;
    rocksdb::WriteOptions wo;
    wo.sync = true;
    auto rs = db_->Put(wo, kKeyNextChunk, pod_to_string(next_chunk_id_));
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

bool MetadataStore::path_exists(const std::string &path)
{
    std::string val;
    auto rs = db_->Get(rocksdb::ReadOptions(), key_path(path), &val);
    return rs.ok();
}

Status MetadataStore::path_lookup(const std::string &path, uint64_t *out_oid)
{
    std::string val;
    auto rs = db_->Get(rocksdb::ReadOptions(), key_path(path), &val);
    if (rs.IsNotFound()) return Status::ERROR_NOT_FOUND;
    if (!rs.ok())        return Status::ERROR_METADATA;
    if (!string_to_pod(val, out_oid)) return Status::ERROR_METADATA;
    return Status::OK;
}

Status MetadataStore::get_attrs(uint64_t oid, ObjectAttrs *out)
{
    std::string val;
    auto rs = db_->Get(rocksdb::ReadOptions(), key_attrs(oid), &val);
    if (rs.IsNotFound()) return Status::ERROR_NOT_FOUND;
    if (!rs.ok())        return Status::ERROR_METADATA;
    if (!decode_attrs(val, out)) return Status::ERROR_METADATA;
    return Status::OK;
}

Status MetadataStore::get_chunk(uint32_t chunk_id, Chunk *out)
{
    std::string val;
    auto rs = db_->Get(rocksdb::ReadOptions(), key_chunk(chunk_id), &val);
    if (rs.IsNotFound()) return Status::ERROR_NOT_FOUND;
    if (!rs.ok())        return Status::ERROR_METADATA;
    if (!decode_chunk(val, out)) return Status::ERROR_METADATA;
    return Status::OK;
}

Status MetadataStore::put_chunk(const Chunk &c)
{
    rocksdb::WriteOptions wo;
    wo.sync = true;
    auto rs = db_->Put(wo, key_chunk(c.chunk_id), encode_chunk(c));
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

/* ----------------------------------------------------------------------
 * Range scan over the extent map for one object.
 *
 * Strategy:
 *   1. Build the prefix key  P = ex:<be64 oid> .
 *   2. Build the lower-bound logical key  L = P + be64(start_offset).
 *   3. SeekForPrev(L): position at the largest key <= L within the
 *      database.  If that key starts with P AND its decoded logical
 *      offset + length covers `start_offset_inclusive`, this is the
 *      first extent of interest.
 *   4. Otherwise advance to the next key (Seek(L)) - the request range
 *      starts inside a hole or at an extent boundary.
 *   5. Iterate forward, decoding each extent, calling cb, until the
 *      key prefix changes (next object) or the logical offset reaches
 *      end_offset_exclusive.
 * ---------------------------------------------------------------------- */
Status MetadataStore::iter_extents(uint64_t oid,
                                    uint64_t start_offset_inclusive,
                                    uint64_t end_offset_exclusive,
                                    const ExtentVisitor &cb)
{
    if (end_offset_exclusive <= start_offset_inclusive) return Status::OK;

    const std::string prefix = key_extent_prefix(oid);
    const std::string lower  = key_extent(oid, start_offset_inclusive);

    rocksdb::ReadOptions ro;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro));

    /* Try to pick up an extent that starts BEFORE start_offset and
     * still covers it. */
    it->SeekForPrev(lower);
    if (it->Valid()) {
        rocksdb::Slice k = it->key();
        if (k.size() == prefix.size() + 8 &&
            std::memcmp(k.data(), prefix.data(), prefix.size()) == 0) {
            uint64_t off = get_be64(k.data() + prefix.size());
            ExtentEntry e{};
            std::string val = it->value().ToString();
            if (!decode_extent(val, &e)) return Status::ERROR_METADATA;
            if (off + e.length > start_offset_inclusive) {
                if (off >= end_offset_exclusive) return Status::OK;
                if (!cb(off, e)) return Status::OK;
                it->Next();
            } else {
                /* Predecessor doesn't cover the range; jump forward. */
                it->Seek(lower);
            }
        } else {
            it->Seek(lower);
        }
    } else {
        it->Seek(lower);
    }

    /* Forward iteration. */
    for (; it->Valid(); it->Next()) {
        rocksdb::Slice k = it->key();
        if (k.size() != prefix.size() + 8) break;
        if (std::memcmp(k.data(), prefix.data(), prefix.size()) != 0) break;

        uint64_t off = get_be64(k.data() + prefix.size());
        if (off >= end_offset_exclusive) break;

        ExtentEntry e{};
        std::string val = it->value().ToString();
        if (!decode_extent(val, &e)) return Status::ERROR_METADATA;

        if (!cb(off, e)) break;
    }
    auto rs = it->status();
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

/* ---- Batch ----------------------------------------------------------- */

MetadataStore::Batch::Batch() : wb_(std::make_unique<rocksdb::WriteBatch>()) {}
MetadataStore::Batch::~Batch() = default;

void MetadataStore::Batch::put_path(const std::string &path, uint64_t oid)
{
    wb_->Put(key_path(path), pod_to_string(oid));
}

void MetadataStore::Batch::put_attrs(const ObjectAttrs &a)
{
    wb_->Put(key_attrs(a.object_id), encode_attrs(a));
}

void MetadataStore::Batch::put_extent(uint64_t oid, uint64_t logical_offset,
                                       const ExtentEntry &e)
{
    wb_->Put(key_extent(oid, logical_offset), encode_extent(e));
}

void MetadataStore::Batch::put_chunk(const Chunk &c)
{
    wb_->Put(key_chunk(c.chunk_id), encode_chunk(c));
}

void MetadataStore::Batch::put_chunk_rev(uint32_t chunk_id, uint16_t idx,
                                          const ChunkRevEntry &r)
{
    wb_->Put(key_chunk_rev(chunk_id, idx), encode_chunk_rev(r));
}

Status MetadataStore::commit(Batch &batch, bool sync)
{
    rocksdb::WriteOptions wo;
    wo.sync = sync;
    auto rs = db_->Write(wo, batch.wb_.get());
    return rs.ok() ? Status::OK : Status::ERROR_METADATA;
}

} /* namespace nebula::objstore */
