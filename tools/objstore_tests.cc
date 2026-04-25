/*
 * objstore_tests.cc - Three integration tests for the ObjectStore.
 *
 *   T1  Multi-write append: two writes on the same object; verify the
 *       DataPointer count doubles and a cross-boundary read returns the
 *       correct concatenation of both writes.
 *
 *   T2  Larger-than-chunk payload: 80 MiB written in one call, which
 *       must seal the first 64 MiB chunk and allocate a fresh second
 *       chunk.  Verifies seal + new-chunk-allocation paths.
 *
 *   T3  Persistence: close the store, reopen it, read both objects
 *       back, and verify content matches.  Validates that
 *       ChunkEngine::open() adopts the still-unsealed second chunk and
 *       that all metadata survived.
 *
 * Usage:
 *   objstore_tests <device_path> <metadata_dir>
 *
 * The device must already be formatted with `nebula_format`.  The
 * metadata_dir is created if missing.  For a clean run, delete both
 * before invoking this tool:
 *
 *   truncate -s 4G /tmp/obj.img
 *   nebula_format --path /tmp/obj.img
 *   rm -rf /tmp/obj_meta
 *   objstore_tests /tmp/obj.img /tmp/obj_meta
 */
#include "objstore/object_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nebula::objstore;

/* ---------------------------------------------------------------------- */
/* Test helpers                                                           */
/* ---------------------------------------------------------------------- */

static int g_failures = 0;

#define EXPECT(cond, ...)                                                 \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "  FAIL [%s:%d] %s -- ",                 \
                         __FILE__, __LINE__, #cond);                      \
            std::fprintf(stderr, __VA_ARGS__);                            \
            std::fprintf(stderr, "\n");                                   \
            ++g_failures;                                                 \
            return false;                                                 \
        }                                                                 \
    } while (0)

#define EXPECT_OK(stmt)                                                   \
    do {                                                                  \
        Status _s = (stmt);                                               \
        if (_s != Status::OK) {                                           \
            std::fprintf(stderr, "  FAIL [%s:%d] %s -> %s\n",             \
                         __FILE__, __LINE__, #stmt, status_str(_s));      \
            ++g_failures;                                                 \
            return false;                                                 \
        }                                                                 \
    } while (0)

/* Deterministic pseudo-random fill so tests are reproducible. */
static void fill_pattern(std::vector<uint8_t> &buf, uint32_t seed)
{
    uint32_t x = seed;
    for (size_t i = 0; i < buf.size(); ++i) {
        x = x * 1103515245u + 12345u;
        buf[i] = static_cast<uint8_t>(x >> 16);
    }
}

static bool buffers_equal(const std::vector<uint8_t> &a,
                          const std::vector<uint8_t> &b,
                          size_t a_off, size_t b_off, size_t n)
{
    if (a.size() < a_off + n) return false;
    if (b.size() < b_off + n) return false;
    return std::memcmp(a.data() + a_off, b.data() + b_off, n) == 0;
}

/* ---------------------------------------------------------------------- */
/* T1: Multi-write append                                                 */
/* ---------------------------------------------------------------------- */
static bool test_multi_write(ObjectStore &store, const std::string &path)
{
    std::printf("T1: multi-write append on %s\n", path.c_str());

    /* Two distinct payloads.  Each fits in one chunk so each write()
     * produces exactly one extent (chunk-bounded extent design). */
    constexpr size_t kHalf = 256 * 1024;     /* 256 KiB -> 1 extent each */
    std::vector<uint8_t> a(kHalf), b(kHalf);
    fill_pattern(a, /*seed=*/0xA1A1A1A1u);
    fill_pattern(b, /*seed=*/0xB2B2B2B2u);

    EXPECT_OK(store.create(path));

    EXPECT_OK(store.write(path, a.data(), a.size()));
    ObjectAttrs md1{};
    EXPECT_OK(store.stat(path, &md1));
    uint64_t extents_after_first = md1.num_extents;
    std::printf("   after 1st write: size=%llu extents=%llu\n",
                (unsigned long long)md1.size,
                (unsigned long long)extents_after_first);
    EXPECT(md1.size == kHalf, "size should equal first write length");

    EXPECT_OK(store.write(path, b.data(), b.size()));
    ObjectAttrs md2{};
    EXPECT_OK(store.stat(path, &md2));
    std::printf("   after 2nd write: size=%llu extents=%llu version=%llu\n",
                (unsigned long long)md2.size,
                (unsigned long long)md2.num_extents,
                (unsigned long long)md2.version);
    EXPECT(md2.size == kHalf * 2, "size should equal sum of writes");
    EXPECT(md2.num_extents == extents_after_first * 2,
           "extent count should double (got %llu vs expected %llu)",
           (unsigned long long)md2.num_extents,
           (unsigned long long)(extents_after_first * 2));
    EXPECT(md2.version == md1.version + 1, "version should advance");

    /* (1) Read full object - expect a || b. */
    std::vector<uint8_t> full;
    EXPECT_OK(store.read(path, 0, kHalf * 2, &full));
    EXPECT(full.size() == kHalf * 2, "full read short");
    EXPECT(buffers_equal(full, a, 0,     0, kHalf), "first half mismatch");
    EXPECT(buffers_equal(full, b, kHalf, 0, kHalf), "second half mismatch");

    /* (2) Read crossing the boundary: middle 8 KiB straddling kHalf. */
    constexpr size_t kCross = 8 * 1024;
    std::vector<uint8_t> cross;
    EXPECT_OK(store.read(path, kHalf - kCross/2, kCross, &cross));
    EXPECT(cross.size() == kCross, "boundary read short (%zu)", cross.size());
    /* First half of `cross` is from `a` tail, second half from `b` head. */
    EXPECT(buffers_equal(cross, a, 0,        kHalf - kCross/2, kCross/2),
           "boundary read: a-tail mismatch");
    EXPECT(buffers_equal(cross, b, kCross/2, 0,                kCross/2),
           "boundary read: b-head mismatch");

    /* (3) Random-offset read entirely in the second write. */
    std::vector<uint8_t> tail;
    const size_t tail_off = kHalf + 1024;
    const size_t tail_len = 4096;
    EXPECT_OK(store.read(path, tail_off, tail_len, &tail));
    EXPECT(tail.size() == tail_len, "tail read short");
    EXPECT(buffers_equal(tail, b, 0, 1024, tail_len), "tail read mismatch");

    std::printf("   T1 PASS\n");
    return true;
}

/* ---------------------------------------------------------------------- */
/* T2: Larger-than-chunk payload                                          */
/* ---------------------------------------------------------------------- */
static bool test_chunk_seal(ObjectStore &store, const std::string &path)
{
    std::printf("T2: large payload + chunk seal on %s\n", path.c_str());

    /* 80 MiB > 64 MiB chunk size, so the first chunk must be sealed
     * mid-write and a second chunk must be allocated to carry the
     * remaining ~16 MiB.  With chunk-bounded extents this produces
     * exactly 2 extents (one per chunk). */
    constexpr size_t kPayload = 80ull * 1024 * 1024;
    std::vector<uint8_t> data(kPayload);
    fill_pattern(data, /*seed=*/0xC3C3C3C3u);

    EXPECT_OK(store.create(path));
    EXPECT_OK(store.write(path, data.data(), data.size()));

    ObjectAttrs md{};
    EXPECT_OK(store.stat(path, &md));
    std::printf("   size=%llu extents=%llu (expect 2 chunk-bounded extents)\n",
                (unsigned long long)md.size,
                (unsigned long long)md.num_extents);
    EXPECT(md.size == kPayload, "size mismatch");
    EXPECT(md.num_extents == 2,
           "expected exactly 2 extents (one per chunk), got %llu",
           (unsigned long long)md.num_extents);

    /* Verify the extent map spans (at least) two distinct chunk_ids -
     * proves the seal+new-allocation actually happened. */
    uint32_t first_chunk = 0, last_chunk = 0;
    EXPECT_OK(store.chunk_id_range(path, &first_chunk, &last_chunk));
    std::printf("   chunk_id range: %u .. %u\n", first_chunk, last_chunk);
    EXPECT(last_chunk > first_chunk,
           "expected at least two chunks (got first=%u last=%u)",
           first_chunk, last_chunk);

    /* Read back in three slices and verify content. */
    auto verify_slice = [&](size_t off, size_t len, const char *label) {
        std::vector<uint8_t> got;
        Status s = store.read(path, off, len, &got);
        if (s != Status::OK) {
            std::fprintf(stderr, "   read(%s) failed: %s\n",
                         label, status_str(s));
            ++g_failures;
            return false;
        }
        if (got.size() != len) {
            std::fprintf(stderr, "   read(%s) short: %zu != %zu\n",
                         label, got.size(), len);
            ++g_failures;
            return false;
        }
        if (!buffers_equal(got, data, 0, off, len)) {
            std::fprintf(stderr, "   read(%s) mismatch\n", label);
            ++g_failures;
            return false;
        }
        return true;
    };

    /* Head, mid (straddling the 64 MiB chunk boundary), tail. */
    if (!verify_slice(0,                    1 * 1024 * 1024, "head 1MiB")) return false;
    if (!verify_slice(63 * 1024 * 1024,     2 * 1024 * 1024, "boundary 2MiB")) return false;
    if (!verify_slice(kPayload - 1024*1024, 1 * 1024 * 1024, "tail 1MiB")) return false;

    std::printf("   T2 PASS\n");
    return true;
}

/* ---------------------------------------------------------------------- */
/* T3: Persistence across close/reopen                                    */
/* ---------------------------------------------------------------------- */
static bool test_persistence(const OpenOptions &opts,
                              const std::string &multi_path,
                              const std::string &large_path)
{
    std::printf("T3: persistence (close + reopen)\n");

    /* Open a fresh ObjectStore against the same device + metadata. */
    ObjectStore store2;
    EXPECT_OK(store2.open(opts));

    /* Multi-write object should still report doubled extent count. */
    ObjectAttrs md_multi{};
    EXPECT_OK(store2.stat(multi_path, &md_multi));
    std::printf("   reopened %s: size=%llu extents=%llu version=%llu\n",
                multi_path.c_str(),
                (unsigned long long)md_multi.size,
                (unsigned long long)md_multi.num_extents,
                (unsigned long long)md_multi.version);
    EXPECT(md_multi.size == 512 * 1024, "T1 object size lost");
    EXPECT(md_multi.num_extents >= 2,   "T1 extent map lost");

    /* Large object should still span multiple chunks. */
    ObjectAttrs md_large{};
    EXPECT_OK(store2.stat(large_path, &md_large));
    uint32_t lo = 0, hi = 0;
    EXPECT_OK(store2.chunk_id_range(large_path, &lo, &hi));
    std::printf("   reopened %s: size=%llu extents=%llu chunks=%u..%u\n",
                large_path.c_str(),
                (unsigned long long)md_large.size,
                (unsigned long long)md_large.num_extents,
                lo, hi);
    EXPECT(md_large.size == 80ull * 1024 * 1024, "T2 object size lost");
    EXPECT(hi > lo, "T2 multi-chunk layout lost");

    /* Read both objects fully and recompute the expected payloads. */
    {
        std::vector<uint8_t> a(256 * 1024), b(256 * 1024);
        fill_pattern(a, 0xA1A1A1A1u);
        fill_pattern(b, 0xB2B2B2B2u);
        std::vector<uint8_t> got;
        EXPECT_OK(store2.read(multi_path, 0, md_multi.size, &got));
        EXPECT(got.size() == md_multi.size, "T1 reread short");
        EXPECT(buffers_equal(got, a, 0,           0, 256 * 1024),
               "T1 reread first half");
        EXPECT(buffers_equal(got, b, 256 * 1024,  0, 256 * 1024),
               "T1 reread second half");
    }
    {
        std::vector<uint8_t> data(80ull * 1024 * 1024);
        fill_pattern(data, 0xC3C3C3C3u);
        /* Sample a few slices instead of the whole 80 MiB to keep
         * the test snappy. */
        auto check = [&](size_t off, size_t len) {
            std::vector<uint8_t> got;
            Status s = store2.read(large_path, off, len, &got);
            if (s != Status::OK || got.size() != len ||
                !buffers_equal(got, data, 0, off, len)) {
                std::fprintf(stderr,
                    "   T3 large reread mismatch at off=%zu len=%zu\n",
                    off, len);
                ++g_failures;
                return false;
            }
            return true;
        };
        if (!check(0,                          64 * 1024)) return false;
        if (!check(63ull * 1024 * 1024,        128 * 1024)) return false;
        if (!check(80ull * 1024 * 1024 - 8192, 8192))      return false;
    }

    /* Append once more after reopen and verify it lands. */
    constexpr size_t kPostReopen = 32 * 1024;
    std::vector<uint8_t> extra(kPostReopen);
    fill_pattern(extra, /*seed=*/0xD4D4D4D4u);
    EXPECT_OK(store2.write(multi_path, extra.data(), extra.size()));

    ObjectAttrs md_after{};
    EXPECT_OK(store2.stat(multi_path, &md_after));
    EXPECT(md_after.size == md_multi.size + kPostReopen,
           "post-reopen append size mismatch");

    std::vector<uint8_t> tail;
    EXPECT_OK(store2.read(multi_path, md_multi.size, kPostReopen, &tail));
    EXPECT(tail.size() == kPostReopen, "post-reopen read short");
    EXPECT(buffers_equal(tail, extra, 0, 0, kPostReopen),
           "post-reopen content mismatch");

    store2.close();
    std::printf("   T3 PASS\n");
    return true;
}

/* ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: %s <device_path> <metadata_dir>\n"
            "  Run T1 (multi-write), T2 (chunk seal), T3 (persistence)\n",
            argv[0]);
        return 2;
    }

    OpenOptions opts;
    opts.device_path   = argv[1];
    opts.metadata_path = argv[2];
    opts.sync_metadata = true;

    /* Strict 4-component paths: /<tenant>/<sub>/<dataset>/<objectid>. */
    const std::string p_multi = "/t1/s1/ds1/multi";
    const std::string p_large = "/t1/s1/ds1/large";

    bool t12_ok = false;
    {
        ObjectStore store;
        Status s = store.open(opts);
        if (s != Status::OK) {
            std::fprintf(stderr, "open failed: %s\n", status_str(s));
            return 1;
        }

        bool t1 = test_multi_write(store, p_multi);
        bool t2 = t1 && test_chunk_seal(store, p_large);
        t12_ok = t1 && t2;

        store.close();   /* important: T3 wants a cold reopen */
    }

    if (t12_ok) test_persistence(opts, p_multi, p_large);

    if (g_failures == 0) {
        std::printf("\n== ALL TESTS PASSED ==\n");
        return 0;
    }
    std::printf("\n== %d FAILURE(S) ==\n", g_failures);
    return 1;
}
