/*
 * objstore_demo.cc - End-to-end smoke test for the object store.
 *
 * Usage:
 *   objstore_demo <device_path> <metadata_dir>
 *
 * The device must already be formatted with `nebula_format`.  The
 * metadata_dir is a directory where RocksDB will live (created if
 * missing).
 *
 * The demo exercises CREATE, WRITE, READ end-to-end, then prints stat
 * info and verifies the read content matches what was written.
 */
#include "objstore/object_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nebula::objstore;

static int die(const char *msg, Status s)
{
    std::fprintf(stderr, "FAIL: %s: %s\n", msg, status_str(s));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: %s <device_path> <metadata_dir> [object_path] [size_bytes]\n",
            argv[0]);
        return 2;
    }

    OpenOptions opts;
    opts.device_path    = argv[1];
    opts.metadata_path  = argv[2];
    opts.sync_metadata  = true;

    /* Strict 4-component path: /<tenant>/<sub>/<dataset>/<objectid>. */
    std::string obj_path = (argc >= 4) ? argv[3] : "/t1/s1/ds1/demo_file";
    size_t      payload  = (argc >= 5) ? std::strtoull(argv[4], nullptr, 10)
                                       : (size_t)(256 * 1024);  /* 256 KB */

    ObjectStore store;
    Status s = store.open(opts);
    if (s != Status::OK) return die("open", s);

    std::printf("== ObjectStore demo ==\n");
    std::printf("  device   : %s\n", opts.device_path.c_str());
    std::printf("  metadata : %s\n", opts.metadata_path.c_str());
    std::printf("  object   : %s\n", obj_path.c_str());
    std::printf("  payload  : %zu bytes\n", payload);

    /* CREATE (idempotent: if it already exists, that's fine for a re-run). */
    s = store.create(obj_path);
    if (s == Status::ERROR_ALREADY_EXISTS) {
        std::printf("CREATE: already exists, reusing\n");
    } else if (s != Status::OK) {
        return die("create", s);
    } else {
        std::printf("CREATE: ok\n");
    }

    /* Build deterministic test data. */
    std::vector<uint8_t> buf(payload);
    for (size_t i = 0; i < buf.size(); i++)
        buf[i] = static_cast<uint8_t>((i * 1103515245u + 12345u) >> 8);

    /* WRITE. */
    s = store.write(obj_path, buf.data(), buf.size());
    if (s != Status::OK) return die("write", s);
    std::printf("WRITE: %zu bytes ok\n", buf.size());

    /* STAT. */
    ObjectAttrs md{};
    s = store.stat(obj_path, &md);
    if (s != Status::OK) return die("stat", s);
    std::printf("STAT : oid=%llu size=%llu version=%llu extents=%llu\n",
                (unsigned long long)md.object_id,
                (unsigned long long)md.size,
                (unsigned long long)md.version,
                (unsigned long long)md.num_extents);

    /* READ full object. */
    std::vector<uint8_t> got;
    s = store.read(obj_path, 0, md.size, &got);
    if (s != Status::OK) return die("read", s);
    std::printf("READ : %zu bytes ok\n", got.size());

    /* Verify only the portion we just wrote.  If the object existed
     * already from a previous run, md.size may exceed buf.size(); in
     * that case the trailing pre-existing bytes are not part of `buf`,
     * so we compare the tail of `got`. */
    size_t cmp_len = buf.size();
    size_t cmp_off = (got.size() >= cmp_len) ? got.size() - cmp_len : 0;
    if (got.size() < cmp_len) {
        std::fprintf(stderr, "FAIL: short read (%zu < %zu)\n",
                     got.size(), cmp_len);
        return 1;
    }
    if (std::memcmp(got.data() + cmp_off, buf.data(), cmp_len) != 0) {
        std::fprintf(stderr, "FAIL: data mismatch\n");
        return 1;
    }
    std::printf("VERIFY: payload matches (%zu bytes)\n", cmp_len);

    store.close();
    std::printf("== OK ==\n");
    return 0;
}
