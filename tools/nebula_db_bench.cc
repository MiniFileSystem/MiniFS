/*
 * nebula_db_bench.cc - RocksDB benchmark tool for NebulaFileSystem.
 *
 * Implements common db_bench workloads without requiring RocksDB benchmark headers.
 *
 * NOTE: MiniFS currently has a fixed inode extent map. Large workloads (10k+ keys)
 * may hit "extent map full" errors during WAL compaction. Use smaller workloads
 * (100-1000 keys) for now. A future Phase D redesign (indirect blocks) will fix this.
 *
 * Usage:
 *   nebula_db_bench <device_path> <benchmark> [options]
 *
 * Benchmarks:
 *   fillseq      - Sequential write (key_0000, key_0001, ...)
 *   overwrite    - Overwrite existing keys
 *   readseq      - Sequential read
 *   readrandom   - Random read
 *   readwhilewriting - Concurrent read + write
 *
 * Options:
 *   --num N           Number of keys (default: 100000)
 *   --value_size N    Value size in bytes (default: 100)
 *   --threads N       Number of threads (default: 1)
 *   --batch_size N    Batch size for writes (default: 100)
 *
 * Examples:
 *   nebula_db_bench /tmp/test.img fillseq --num 100000 --value_size 4096
 *   nebula_db_bench /tmp/test.img readrandom --num 10000 --threads 4
 */
#include "../src/rocksdb/nebula_rocksdb_fs.h"

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/write_batch.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <random>
#include <atomic>
#include <cstring>

static void die(const std::string &msg)
{
    std::cerr << "[FAIL] " << msg << "\n";
    std::exit(1);
}

struct BenchConfig {
    int num = 100000;
    int value_size = 100;
    int threads = 1;
    int batch_size = 100;
};

static BenchConfig parse_args(int argc, char **argv)
{
    BenchConfig cfg;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--num") && i + 1 < argc)
            cfg.num = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--value_size") && i + 1 < argc)
            cfg.value_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            cfg.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batch_size") && i + 1 < argc)
            cfg.batch_size = atoi(argv[++i]);
    }
    return cfg;
}

static std::string gen_key(int n)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "key_%08d", n);
    return std::string(buf);
}

static std::string gen_value(int size)
{
    return std::string(size, 'x');
}

/* Benchmark: sequential write */
static void bench_fillseq(rocksdb::DB *db, const BenchConfig &cfg)
{
    rocksdb::WriteOptions wopts;
    wopts.disableWAL = false;

    std::string value = gen_value(cfg.value_size);
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.num; i++) {
        std::string key = gen_key(i);
        rocksdb::Status s = db->Put(wopts, key, value);
        if (!s.ok()) die("Put failed: " + s.ToString());
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double ops_sec = (cfg.num * 1000.0) / (ms + 1);
    double mb_sec = (cfg.num * cfg.value_size / 1024.0 / 1024.0) / (ms / 1000.0 + 0.001);

    std::cout << "fillseq: " << cfg.num << " ops in " << ms << " ms\n";
    std::cout << "  " << ops_sec << " ops/sec, " << mb_sec << " MB/sec\n";
}

/* Benchmark: overwrite */
static void bench_overwrite(rocksdb::DB *db, const BenchConfig &cfg)
{
    rocksdb::WriteOptions wopts;
    std::string value = gen_value(cfg.value_size);
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.num; i++) {
        std::string key = gen_key(i);
        rocksdb::Status s = db->Put(wopts, key, value);
        if (!s.ok()) die("Put failed: " + s.ToString());
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double ops_sec = (cfg.num * 1000.0) / (ms + 1);

    std::cout << "overwrite: " << cfg.num << " ops in " << ms << " ms\n";
    std::cout << "  " << ops_sec << " ops/sec\n";
}

/* Benchmark: sequential read */
static void bench_readseq(rocksdb::DB *db, const BenchConfig &cfg)
{
    rocksdb::ReadOptions ropts;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.num; i++) {
        std::string key = gen_key(i);
        std::string value;
        rocksdb::Status s = db->Get(ropts, key, &value);
        if (!s.ok()) die("Get failed: " + s.ToString());
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double ops_sec = (cfg.num * 1000.0) / (ms + 1);

    std::cout << "readseq: " << cfg.num << " ops in " << ms << " ms\n";
    std::cout << "  " << ops_sec << " ops/sec\n";
}

/* Benchmark: random read */
static void bench_readrandom(rocksdb::DB *db, const BenchConfig &cfg)
{
    rocksdb::ReadOptions ropts;
    std::vector<int> keys(cfg.num);
    for (int i = 0; i < cfg.num; i++) keys[i] = i;
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(keys.begin(), keys.end(), g);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < cfg.num; i++) {
        std::string key = gen_key(keys[i]);
        std::string value;
        rocksdb::Status s = db->Get(ropts, key, &value);
        if (!s.ok()) die("Get failed: " + s.ToString());
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double ops_sec = (cfg.num * 1000.0) / (ms + 1);

    std::cout << "readrandom: " << cfg.num << " ops in " << ms << " ms\n";
    std::cout << "  " << ops_sec << " ops/sec\n";
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: nebula_db_bench <device_path> <benchmark> [options]\n";
        std::cerr << "Benchmarks: fillseq, overwrite, readseq, readrandom\n";
        std::cerr << "Options: --num N, --value_size N, --threads N, --batch_size N\n";
        return 1;
    }

    const std::string device = argv[1];
    const std::string bench_name = argv[2];
    BenchConfig cfg = parse_args(argc, argv);

    std::cout << "Nebula RocksDB Benchmark\n";
    std::cout << "  device: " << device << "\n";
    std::cout << "  benchmark: " << bench_name << "\n";
    std::cout << "  num: " << cfg.num << ", value_size: " << cfg.value_size << "\n\n";

    /* --- Mount Nebula device --- */
    std::shared_ptr<rocksdb::FileSystem> nebula_fs;
    try {
        nebula_fs = std::make_shared<nebula::NebulaFileSystem>(device);
    } catch (const std::exception &e) {
        die("Mount failed: " + std::string(e.what()));
    }
    std::cout << "[OK] Nebula device mounted\n";

    /* --- Wrap in Env --- */
    std::unique_ptr<rocksdb::Env> nebula_env = rocksdb::NewCompositeEnv(nebula_fs);

    /* --- Open RocksDB --- */
    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.env = nebula_env.get();
    opts.write_buffer_size = 64 << 20;  /* 64MB */
    opts.max_write_buffer_number = 3;

    rocksdb::DB *db_raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(opts, "/nebuladb", &db_raw);
    if (!s.ok()) die("DB::Open failed: " + s.ToString());
    std::unique_ptr<rocksdb::DB> db(db_raw);
    std::cout << "[OK] RocksDB opened\n\n";

    /* --- Run benchmark --- */
    if (bench_name == "fillseq") {
        bench_fillseq(db.get(), cfg);
    } else if (bench_name == "overwrite") {
        bench_overwrite(db.get(), cfg);
    } else if (bench_name == "readseq") {
        bench_readseq(db.get(), cfg);
    } else if (bench_name == "readrandom") {
        bench_readrandom(db.get(), cfg);
    } else {
        die("Unknown benchmark: " + bench_name);
    }

    std::cout << "\n[OK] Benchmark completed\n";
    return 0;
}
