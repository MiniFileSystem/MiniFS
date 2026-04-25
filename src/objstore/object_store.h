/*
 * objstore/object_store.h - Public API for the multi-tenant object store.
 *
 * Path layout (enforced):
 *   /<tenant>/<subtenant>/<dataset>/<objectid>
 *
 * Implements:
 *   CREATE: validate path -> register attrs in RocksDB (zero device I/O)
 *   WRITE:  validate path -> for each segment: reserve chunk slot ->
 *           sync write -> stage (ex:, ci:) inserts in batch ->
 *           durability barrier (sync writes already returned) ->
 *           commit batch (attrs, ex:*, ci:*, ck:active) atomically
 *   READ:   validate path -> attrs.size clamp -> RocksDB iterator
 *           prefix scan over ex:<be64 oid> for [offset, offset+size) ->
 *           per-extent: cache chunk lookup, nebula_io_read, copy slice
 */
#ifndef NEBULA_OBJSTORE_OBJECT_STORE_H
#define NEBULA_OBJSTORE_OBJECT_STORE_H

#include "common.h"

#include <memory>
#include <string>
#include <vector>

extern "C" {
struct nebula_mount;
struct nebula_io;
}

namespace nebula::objstore {

class MetadataStore;
class ChunkEngine;

struct OpenOptions {
    std::string device_path;
    std::string metadata_path;
    bool sync_metadata = true;
};

class ObjectStore {
public:
    ObjectStore();
    ~ObjectStore();

    Status open(const OpenOptions &opts);
    void   close();

    /* CREATE - register a new object at strict path
     * /<tenant>/<sub>/<ds>/<oid>. */
    Status create(const std::string &path);

    /* WRITE - append `len` bytes to the object at `path`. */
    Status write(const std::string &path, const void *data, size_t len);

    /* READ - read up to `size` bytes starting at logical `offset`.
     * Resizes `out` to the actual number of bytes read. */
    Status read(const std::string &path, uint64_t offset, size_t size,
                std::vector<uint8_t> *out);

    /* READ (raw buffer overload) - read up to `dst_cap` bytes starting at
     * logical `offset` into a caller-provided buffer.  Returns the actual
     * number of bytes read in *out_bytes (clamped by object size and
     * dst_cap).  This avoids the vector zero-fill / page-fault cost of
     * the std::vector overload and is the recommended hot-path API. */
    Status read(const std::string &path, uint64_t offset, size_t dst_cap,
                void *dst, size_t *out_bytes);

    /* Stat - returns the attribute portion (no extent list). */
    Status stat(const std::string &path, ObjectAttrs *out);

    /* Diagnostic: scan the object's extent map and report the smallest
     * and largest chunk_id used.  Useful for tests / debugging.  Not
     * intended for hot paths - it walks every extent. */
    Status chunk_id_range(const std::string &path,
                          uint32_t *out_min, uint32_t *out_max);

private:
    ObjectStore(const ObjectStore &) = delete;
    ObjectStore &operator=(const ObjectStore &) = delete;

    Status write_blocks(uint64_t physical_lba, const void *buf,
                        uint32_t n_blocks);
    Status read_blocks(uint64_t physical_lba, void *buf, uint32_t n_blocks);

    OpenOptions                    opts_;
    struct nebula_mount           *mount_  = nullptr;
    std::unique_ptr<MetadataStore> meta_;
    std::unique_ptr<ChunkEngine>   chunks_;
};

} /* namespace nebula::objstore */

#endif /* NEBULA_OBJSTORE_OBJECT_STORE_H */
