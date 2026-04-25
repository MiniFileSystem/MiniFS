# ObjectStore — Multi-Tenant KV-Style API on MiniFS

This document describes the **ObjectStore** layer added to MiniFS. It
implements the *Multi-Tenant Object Store — Implementation Design*
(CREATE / WRITE / READ via TXG; chunk-based, append-only data plane;
metadata in RocksDB) on top of the existing Nebula block allocator and
I/O backends.

## 1. Layering

```
+--------------------------------------------------------------+
|  Caller (test tool, future SDK)                              |
+--------------------------------------------------------------+
|  ObjectStore::create / write / read   (src/objstore)         |
+----------------+--------------------+------------------------+
| MetadataStore  |   ChunkEngine      |  Block-IO helpers      |
| (RocksDB)      |  (active chunk +   |  nebula_io_read/write  |
|                |   reserve/seal)    |                        |
+----------------+----------+---------+-----------+------------+
                            v                     v
                  nebula_block_alloc ()    nebula_io (POSIX or SPDK)
                            v                     v
                          Nebula on-disk format (formatted device)
```

The data path bypasses the legacy MiniFS inode/directory code entirely.
The block allocator and the I/O backend (`POSIX` today, `SPDK` when
`-DNEBULA_ENABLE_SPDK=ON`) are reused.

## 2. Module Layout

```
src/objstore/
├── common.h / common.cc       - structs, status codes, serialization
├── metadata.h / metadata.cc   - RocksDB wrapper + WriteBatch (TXG)
├── chunk.h   / chunk.cc       - chunk lifecycle, reserve(), seal()
└── object_store.h / .cc       - public CREATE / WRITE / READ API

tools/objstore_demo.cc         - end-to-end smoke test binary
```

Build target: `nebula_objstore` (only when `-DNEBULA_ENABLE_ROCKSDB=ON`).

## 3. Data Structures (schema v2 — extent-keyed)

| Key                                       | Value             | Purpose |
|-------------------------------------------|-------------------|---------|
| `ph:<path>`                               | `uint64 oid`      | path → object id |
| `at:<be64 oid>`                           | `ObjectAttrs`     | size, ctime, mtime, version, num_extents (NO inline pointer list) |
| `ex:<be64 oid><be64 logical_offset>`      | `ExtentEntry`     | forward map: `{chunk_id, chunk_idx, offset_in_chunk, length, crc}` — one key per append segment |
| `ck:<be32 chunk_id>`                      | `Chunk`           | 64 MiB region: `{lba_start, write_offset, sealed}` |
| `ci:<be32 chunk_id><be16 idx>`            | `ChunkRevEntry`   | reverse map for GC: `{oid, logical_offset, length}` |
| `ct:next_object_id` / `ct:next_chunk_id`  | counters          | monotonic id allocators |
| `ct:schema_version`                       | `uint32`          | refuses cross-version mounts |

**Why big-endian numeric key components:** RocksDB sorts keys lexicographically. Encoding `oid` and `logical_offset` in big-endian makes byte-wise lex order identical to numeric order, so a single `Iterator::Seek` + forward walk yields exactly the extents covering any logical byte range.

**Path layout (enforced):** `/<tenant>/<subtenant>/<dataset>/<objectid>` — exactly four non-empty components.

## 4. Global Rules (enforced)

1. **No overwrite.** Each segment claims a fresh, block-aligned region
   inside the active chunk. The chunk's `write_offset` only ever grows.
2. **Data durable before metadata.** Each segment is written
   synchronously through `nebula_io_write()` before any metadata
   describing it enters the RocksDB write batch.
3. **Metadata never points to non-durable data.** Metadata is committed
   only after every data write in the TXG has returned successfully.
4. **Chunk sealing.** When the active chunk cannot fit the next
   segment, it is sealed (record marked `sealed=true`) and a fresh
   64 MiB chunk is allocated.
5. **All writes go through TXG.** `MetadataStore::Batch` is the TXG
   boundary; one `commit(batch)` call atomically updates the path map,
   object metadata, chunk record, and chunk index entries.

## 5. CREATE Flow

```
ObjectStore::create("/foo")
  parent = "/"
  if !meta.path_exists(parent)        -> ERROR_PARENT_NOT_FOUND
  if  meta.path_exists("/foo")        -> ERROR_ALREADY_EXISTS
  oid = meta.allocate_object_id()
  build ObjectMetadata{oid, size=0, version=1, ptrs={}}
  Batch:
    ob:<oid> = encoded(metadata)
    ph:/foo  = oid
  meta.commit(batch, sync=true)
```

No data path is touched. One RocksDB write batch (path entry +
metadata + counter persist).

## 6. WRITE Flow

```
ObjectStore::write("/foo", data, len)
  oid = meta.path_lookup("/foo")
  md  = meta.get_object(oid)

  for each 64 KiB segment:
    r = chunk_engine.reserve(seg_len)        # may seal + allocate new chunk
    pad scratch buffer to r.padded_bytes
    crc = crc32c(scratch, padded)
    nebula_io_write(r.physical_lba, scratch) # SYNC: durability barrier
    Batch.put_chunk_index_entry(r.chunk_id, r.chunk_idx, offset, length, crc)
    md.ptrs.append({timestamp, r.chunk_id, r.chunk_idx, r.length_bytes})

  md.size    += len
  md.version += 1
  md.mtime   = now()
  Batch.put_object(md)
  Batch.put_chunk(active_chunk)              # write_offset / next_idx update
  meta.commit(batch, sync=true)
```

If any `nebula_io_write()` fails, the function returns without
committing the batch. The bytes already written become orphan data
inside the chunk; future GC reclaims them.

## 7. READ Flow

```
ObjectStore::read("/foo", offset, size)
  oid = meta.path_lookup("/foo")
  md  = meta.get_object(oid)
  clamp size to md.size - offset
  for each DataPointer p in md.ptrs:
     if offset still consumes p.length:
         skip
     else:
         idx_entry = meta.get_chunk_index_entry(p.chunk_id, p.chunk_idx)
         chunk     = meta.get_chunk(p.chunk_id)
         phys_lba  = chunk.lba_start + idx_entry.offset_bytes / 4096
         nebula_io_read(phys_lba, padded scratch)
         copy scratch[in_seg_off .. in_seg_off+to_copy] -> output
```

No caching, no prefetch (yet); each segment becomes one underlying
read request. A future optimization is per-chunk readahead.

## 8. Phase 1+2 Simplifications

These are deliberate scope cuts — each has a clear future fix:

| Limitation | Why kept simple | Future fix |
|------------|-----------------|------------|
| Per-segment writes are 4 KiB-padded | Keeps writes truly append-only | Pack sub-block tails into the next segment |
| Index entries persisted per-append | Simpler than chunk footers | Write footer on seal; drop per-entry puts |
| Synchronous metadata commit | Easier reasoning | Group commits per TXG window (rule 5 already supports this) |
| Recovery only adopts unsealed chunks | No full crash recovery yet | Stream log + uberblock-driven replay (M6) |
| No GC | Phase 1+2 scope | `/gc/` module to compact orphan ranges |
| No multi-tenant keyspace prefix | Path is opaque string | `/<slice>/<tenant>/<subtenant>/<dataset>/...` is just a longer path |

## 9. Build & Run

```bash
# (One-time) format an image:
truncate -s 4G /tmp/obj.img
~/nebula-build/nebula_format --path /tmp/obj.img

# Build with RocksDB enabled:
cmake -B ~/nebula-build -S /mnt/c/Users/Shailendra/CascadeProjects/MiniFS \
      -DNEBULA_ENABLE_ROCKSDB=ON
cmake --build ~/nebula-build -j

# Run end-to-end demo (creates RocksDB metadata at /tmp/obj_meta):
~/nebula-build/objstore_demo /tmp/obj.img /tmp/obj_meta /demo 1048576
```

Expected output:
```
== ObjectStore demo ==
  device   : /tmp/obj.img
  metadata : /tmp/obj_meta
  object   : /demo
  payload  : 1048576 bytes
CREATE: ok
WRITE: 1048576 bytes ok
STAT : oid=1 size=1048576 version=2 pointers=16
READ : 1048576 bytes ok
VERIFY: payload matches (1048576 bytes)
== OK ==
```

## 10. Mapping to the Original Spec

| Spec module | Code location | Notes |
|-------------|---------------|-------|
| `/metadata/`  | `src/objstore/metadata.{h,cc}` | RocksDB wrapper, WriteBatch = TXG |
| `/chunk/`     | `src/objstore/chunk.{h,cc}`    | reserve/seal/get_index |
| `/allocator/` | `nebula_block_alloc` (reused) | Stream log not yet plumbed in here |
| `/io/`        | `nebula_io_*` (reused)         | POSIX or SPDK |
| `/txg/`       | `MetadataStore::Batch`         | Atomic RocksDB write |
| `/api/`       | `src/objstore/object_store.{h,cc}` | CREATE / WRITE / READ |
| `/gc/`        | not yet implemented           | Phase 3 |
| `/recovery/`  | `ChunkEngine::open()` (partial)| Adopt unsealed chunk only |
| `/common/`    | `src/objstore/common.{h,cc}`   | Structs + serialization |

## 11. Mental Model (one-liner each)

- **CREATE** = one RocksDB write batch, zero device I/O.
- **WRITE**  = N synchronous block writes (one per padded segment),
  followed by one RocksDB write batch.
- **READ**   = N synchronous block reads (one per DataPointer touched),
  zero RocksDB writes.
