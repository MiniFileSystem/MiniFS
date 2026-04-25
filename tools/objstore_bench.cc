/*
 * objstore_bench.cc - Apples-to-apples performance comparison.
 *
 *   Path A: ObjectStore write/read
 *           - 4 KiB-padded segments through nebula_io_write()
 *           - one RocksDB WriteBatch commit per public write() call
 *           - data fsync (Step 3 barrier) + RocksDB WAL fsync at commit
 *
 *   Path B: Raw pwrite/pread baseline
 *           - same total bytes, same segment size
 *           - O_DIRECT-friendly aligned buffer
 *           - one fdatasync() at the end of the write phase
 *           - no metadata at all - this is the "physical floor"
 *
 *  Both paths are timed end-to-end including their respective
 *  durability barriers, so the gap between them is exactly the cost of
 *  the ObjectStore metadata plane (extent inserts + RocksDB commit).
 *
 *  Usage:
 *    objstore_bench <device_path> <metadata_dir> <baseline_file>
 *                   [total_bytes=64M] [segment_bytes=64K]
 *
 *  Notes:
 *    - <device_path> must already be formatted with `nebula_format`.
 *    - <baseline_file> is a plain file used by Path B; created/truncated.
 *    - For the fairest hardware comparison, point both at the same
 *      physical disk (e.g. /tmp on tmpfs vs /tmp on ext4 will skew).
 */
#include "objstore/object_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nebula::objstore;
using clk = std::chrono::steady_clock;

static double secs_since(clk::time_point t0)
{
    auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                  clk::now() - t0);
    return dt.count();
}

static double mib_per_sec(size_t bytes, double seconds)
{
    if (seconds <= 0) return 0.0;
    return (double)bytes / (1024.0 * 1024.0) / seconds;
}

static void *aligned_buf(size_t bytes)
{
    void *p = nullptr;
    if (posix_memalign(&p, 4096, bytes) != 0) return nullptr;
    return p;
}

/* ---------------------------------------------------------------------- */
/* Path A: ObjectStore                                                    */
/* ---------------------------------------------------------------------- */
static int run_objstore(const OpenOptions &opts,
                         size_t total_bytes,
                         size_t /*seg_bytes (handled internally)*/,
                         double *out_write_secs, double *out_read_secs)
{
    ObjectStore store;
    Status s = store.open(opts);
    if (s != Status::OK) {
        std::fprintf(stderr, "ObjectStore::open failed: %s\n", status_str(s));
        return -1;
    }

    const std::string path = "/bench/dataset/run/obj0";

    s = store.create(path);
    if (s != Status::OK && s != Status::ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "ObjectStore::create failed: %s\n", status_str(s));
        return -1;
    }

    /* Build payload. */
    std::vector<uint8_t> payload(total_bytes);
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = static_cast<uint8_t>(i ^ (i >> 8));

    /* WRITE phase: single store.write() call.  Internally this
     * iterates segments, each going through nebula_io_write(), then
     * one fdatasync (data) + one RocksDB sync commit (metadata). */
    auto t0 = clk::now();
    s = store.write(path, payload.data(), payload.size());
    *out_write_secs = secs_since(t0);
    if (s != Status::OK) {
        std::fprintf(stderr, "ObjectStore::write failed: %s\n", status_str(s));
        return -1;
    }

    /* READ phase: full object read into a pre-allocated, pre-warmed
     * raw buffer.  This mirrors how the baseline path reuses a single
     * aligned buf across all preads - no per-read allocation, no
     * anonymous-page faults during the timed window. */
    void *got = aligned_buf(total_bytes);
    if (!got) { std::fprintf(stderr, "aligned_buf failed\n"); return -1; }
    /* Pre-touch every page so the timed read sees no first-touch
     * page faults (baseline buf is naturally hot from the write loop). */
    std::memset(got, 0, total_bytes);

    size_t got_bytes = 0;
    auto t1 = clk::now();
    s = store.read(path, 0, total_bytes, got, &got_bytes);
    *out_read_secs = secs_since(t1);
    if (s != Status::OK) {
        std::fprintf(stderr, "ObjectStore::read failed: %s\n", status_str(s));
        std::free(got);
        return -1;
    }
    if (got_bytes != total_bytes ||
        std::memcmp(got, payload.data(), total_bytes) != 0) {
        std::fprintf(stderr, "ObjectStore: read content mismatch\n");
        std::free(got);
        return -1;
    }
    std::free(got);

    store.close();
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Path B: Raw pwrite/pread baseline                                      */
/* ---------------------------------------------------------------------- */
static int run_baseline(const std::string &file_path,
                         size_t total_bytes, size_t seg_bytes,
                         double *out_write_secs, double *out_read_secs)
{
    /* Truncate first so we measure write-from-empty, matching the
     * ObjectStore case (fresh chunk allocation). */
    int fd = open(file_path.c_str(),
                  O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "baseline: open(%s) failed: %s\n",
                     file_path.c_str(), strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)total_bytes) != 0) {
        std::fprintf(stderr, "baseline: ftruncate failed: %s\n",
                     strerror(errno));
        close(fd);
        return -1;
    }

    void *buf = aligned_buf(seg_bytes);
    if (!buf) { close(fd); return -1; }
    /* Same-ish payload pattern as Path A so we don't get compression
     * surprises if the device path happens to be on a compressing FS. */
    uint8_t *bb = (uint8_t *)buf;
    for (size_t i = 0; i < seg_bytes; i++)
        bb[i] = static_cast<uint8_t>(i ^ (i >> 8));

    /* WRITE phase. */
    auto t0 = clk::now();
    size_t off = 0;
    while (off < total_bytes) {
        size_t this_seg = std::min<size_t>(seg_bytes, total_bytes - off);
        ssize_t w = pwrite(fd, buf, this_seg, (off_t)off);
        if (w < 0) {
            std::fprintf(stderr, "baseline: pwrite failed: %s\n",
                         strerror(errno));
            std::free(buf); close(fd); return -1;
        }
        if ((size_t)w != this_seg) {
            std::fprintf(stderr, "baseline: short pwrite: %zd != %zu\n",
                         w, this_seg);
            std::free(buf); close(fd); return -1;
        }
        off += this_seg;
    }
    /* Durability barrier (matches ObjectStore Step 3). */
    if (fdatasync(fd) != 0) {
        std::fprintf(stderr, "baseline: fdatasync failed: %s\n",
                     strerror(errno));
        std::free(buf); close(fd); return -1;
    }
    *out_write_secs = secs_since(t0);

    /* READ phase. */
    auto t1 = clk::now();
    off = 0;
    while (off < total_bytes) {
        size_t this_seg = std::min<size_t>(seg_bytes, total_bytes - off);
        ssize_t r = pread(fd, buf, this_seg, (off_t)off);
        if (r < 0) {
            std::fprintf(stderr, "baseline: pread failed: %s\n",
                         strerror(errno));
            std::free(buf); close(fd); return -1;
        }
        if ((size_t)r != this_seg) {
            std::fprintf(stderr, "baseline: short pread: %zd != %zu\n",
                         r, this_seg);
            std::free(buf); close(fd); return -1;
        }
        off += this_seg;
    }
    *out_read_secs = secs_since(t1);

    std::free(buf);
    close(fd);
    return 0;
}

/* ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr,
            "Usage: %s <device_path> <metadata_dir> <baseline_file>\n"
            "         [total_bytes=64M] [segment_bytes=64K]\n",
            argv[0]);
        return 2;
    }

    OpenOptions opts;
    opts.device_path    = argv[1];
    opts.metadata_path  = argv[2];
    opts.sync_metadata  = true;

    const std::string baseline_file = argv[3];

    auto parse_size = [](const char *s, size_t dflt) -> size_t {
        if (!s) return dflt;
        char *end = nullptr;
        unsigned long long v = std::strtoull(s, &end, 10);
        if (!end) return dflt;
        switch (*end) {
        case 'K': case 'k': v *= 1024ULL;            break;
        case 'M': case 'm': v *= 1024ULL * 1024;     break;
        case 'G': case 'g': v *= 1024ULL * 1024 * 1024; break;
        default: break;
        }
        return (size_t)v;
    };

    size_t total_bytes = parse_size(argc >= 5 ? argv[4] : nullptr,
                                    64ULL * 1024 * 1024);    /* 64 MiB */
    size_t seg_bytes   = parse_size(argc >= 6 ? argv[5] : nullptr,
                                    64ULL * 1024);            /* 64 KiB */

    std::printf("=========================================================\n");
    std::printf("  ObjectStore vs raw pwrite/pread benchmark\n");
    std::printf("=========================================================\n");
    std::printf("  device       : %s\n", opts.device_path.c_str());
    std::printf("  metadata     : %s\n", opts.metadata_path.c_str());
    std::printf("  baseline file: %s\n", baseline_file.c_str());
    std::printf("  total bytes  : %zu (%.2f MiB)\n",
                total_bytes, total_bytes / 1048576.0);
    std::printf("  segment      : %zu (%.0f KiB)\n",
                seg_bytes, seg_bytes / 1024.0);
    std::printf("---------------------------------------------------------\n");

    double a_write = 0, a_read = 0;
    if (run_objstore(opts, total_bytes, seg_bytes, &a_write, &a_read) != 0)
        return 1;

    double b_write = 0, b_read = 0;
    if (run_baseline(baseline_file, total_bytes, seg_bytes,
                     &b_write, &b_read) != 0)
        return 1;

    double a_write_mibs = mib_per_sec(total_bytes, a_write);
    double a_read_mibs  = mib_per_sec(total_bytes, a_read);
    double b_write_mibs = mib_per_sec(total_bytes, b_write);
    double b_read_mibs  = mib_per_sec(total_bytes, b_read);

    /* +ve => ObjectStore faster than baseline, -ve => slower. */
    auto delta_pct = [](double a, double b) -> double {
        if (b <= 0) return 0;
        return 100.0 * (a - b) / b;
    };
    auto ratio = [](double a, double b) -> double {
        return (a > 0) ? b / a : 0.0;
    };

    std::printf("\n");
    std::printf("                       time(s)   throughput(MiB/s)\n");
    std::printf("  Baseline pwrite  :  %7.4f       %9.2f\n",
                b_write, b_write_mibs);
    std::printf("  ObjectStore write:  %7.4f       %9.2f   (%+.1f%% vs baseline)\n",
                a_write, a_write_mibs,
                delta_pct(a_write_mibs, b_write_mibs));
    std::printf("\n");
    std::printf("  Baseline pread   :  %7.4f       %9.2f\n",
                b_read, b_read_mibs);
    std::printf("  ObjectStore read :  %7.4f       %9.2f   (%+.1f%% vs baseline)\n",
                a_read, a_read_mibs,
                delta_pct(a_read_mibs, b_read_mibs));

    std::printf("\n");
    std::printf("  Overhead (write) :  %.2fx slower than raw pwrite\n",
                ratio(a_write_mibs, b_write_mibs));
    std::printf("  Overhead (read)  :  %.2fx slower than raw pread\n",
                ratio(a_read_mibs, b_read_mibs));
    std::printf("=========================================================\n");
    return 0;
}
