/*
 * nebula_rocksdb_test.cc - C7: Smoke test for the Nebula RocksDB adapter.
 *
 * Usage:
 *   nebula_rocksdb_test <device_image>            (POSIX backend)
 *   nebula_rocksdb_test --spdk <pcie-bdf>          (SPDK NVMe backend)
 *   nebula_rocksdb_test --count 10000 <image>      (custom key count)
 *   nebula_rocksdb_test --value-size 4096 <image>  (custom value size)
 *
 * What it does:
 *   1. Mounts the Nebula device via NebulaFileSystem.
 *   2. Opens a RocksDB database on top of it.
 *   3. Writes N key-value pairs.
 *   4. Reads them back and verifies.
 *   5. Closes the DB cleanly.
 */
#include "../src/rocksdb/nebula_rocksdb_fs.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/env.h>

#include <iostream>
#include <string>
#include <memory>

#ifdef NEBULA_ENABLE_SPDK
extern "C" {
#include "../src/io/nebula_spdk_env.h"
#include "../src/io/nebula_io_spdk.h"
}
#endif

static void die(const std::string &msg, const rocksdb::Status &s)
{
    std::cerr << "[FAIL] " << msg << ": " << s.ToString() << "\n";
    std::exit(1);
}

int main(int argc, char **argv)
{
    const char *device  = nullptr;
    bool        use_spdk = false;
    int         count    = 10;
    int         val_size = 8;   /* bytes */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spdk"))                       use_spdk = true;
        else if (!strcmp(argv[i], "--count") && i+1 < argc)  count    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--value-size") && i+1 < argc) val_size = atoi(argv[++i]);
        else if (!device)                                     device   = argv[i];
        else { std::cerr << "Unknown arg: " << argv[i] << "\n"; return 2; }
    }

    if (!device) {
        std::cerr << "Usage: nebula_rocksdb_test [--spdk] [--count N]"
                     " [--value-size B] <device>\n";
        return 1;
    }

    /* --- Optional SPDK initialisation --- */
    struct nebula_io *spdk_io = nullptr;
    (void)spdk_io;

    if (use_spdk) {
#ifndef NEBULA_ENABLE_SPDK
        std::cerr << "[FAIL] --spdk requires -DNEBULA_ENABLE_SPDK=ON\n";
        return 1;
#else
        if (nebula_spdk_env_init(nullptr) != 0) {
            std::cerr << "[FAIL] SPDK env init failed\n"; return 1;
        }
        if (nebula_io_spdk_open(device, &spdk_io) != 0) {
            std::cerr << "[FAIL] SPDK open " << device << " failed\n";
            nebula_spdk_env_fini(); return 1;
        }
        std::cout << "[OK] SPDK NVMe opened: " << device << "\n";
#endif
    }

    /* --- Mount Nebula device via our FileSystem adapter --- */
    std::shared_ptr<rocksdb::FileSystem> nebula_fs;
    try {
#ifdef NEBULA_ENABLE_SPDK
        if (use_spdk) {
            nebula_fs = std::make_shared<nebula::NebulaFileSystem>(spdk_io);
        } else
#endif
        {
            nebula_fs = std::make_shared<nebula::NebulaFileSystem>(
                std::string(device));
        }
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] Mount failed: " << e.what() << "\n";
#ifdef NEBULA_ENABLE_SPDK
        if (use_spdk) nebula_spdk_env_fini();
#endif
        return 1;
    }
    std::cout << "[OK] Nebula device mounted: " << device << "\n";

    /* --- Open RocksDB on top of Nebula --- */
    /* NewCompositeEnv wraps our FileSystem inside a full Env.
     * Keep the unique_ptr alive for the lifetime of the DB. */
    std::unique_ptr<rocksdb::Env> nebula_env =
        rocksdb::NewCompositeEnv(nebula_fs);

    rocksdb::Options opts;
    opts.create_if_missing = true;
    opts.env = nebula_env.get();
    /* Use lower-level Open to get the actual error instead of an assert */
    rocksdb::DBOptions db_opts(opts);
    rocksdb::ColumnFamilyOptions cf_opts(opts);
    std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
    cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, cf_opts);
    std::vector<rocksdb::ColumnFamilyHandle *> handles;

    rocksdb::DB *db_raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(db_opts, "/nebuladb", cfs,
                                          &handles, &db_raw);
    if (!s.ok()) die("DB::Open", s);
    if (handles.empty()) { std::cerr << "[FAIL] DB opened but no CF handle\n"; return 1; }
    delete handles[0];

    std::unique_ptr<rocksdb::DB> db(db_raw);
    std::cout << "[OK] RocksDB opened on Nebula\n";

    /* --- Write N key-value pairs --- */
    std::string val_template(val_size, 'x');
    rocksdb::WriteOptions wo;
    for (int i = 0; i < count; i++) {
        std::string key = "key_" + std::to_string(i);
        /* Embed index in value so we can verify it */
        std::string val = std::to_string(i) + "_" +
                          val_template.substr(0, std::max(0, val_size - 12));
        s = db->Put(wo, key, val);
        if (!s.ok()) die("Put " + key, s);
    }
    std::cout << "[OK] Wrote " << count << " key-value pairs"
              << " (value_size~" << val_size << "B)\n";

    /* --- Read back and verify --- */
    rocksdb::ReadOptions ro;
    int ok = 0;
    for (int i = 0; i < count; i++) {
        std::string key = "key_" + std::to_string(i);
        std::string got;
        s = db->Get(ro, key, &got);
        if (!s.ok()) {
            std::cerr << "[FAIL] Get " << key << ": " << s.ToString() << "\n";
            continue;
        }
        /* Check prefix contains the index */
        std::string prefix = std::to_string(i) + "_";
        if (got.substr(0, prefix.size()) == prefix) {
            ok++;
        } else {
            std::cerr << "[FAIL] " << key << " value corrupted\n";
        }
    }
    std::cout << "[OK] Read back: " << ok << "/" << count << " correct\n";

    /* --- Close --- */
    db.reset();
#ifdef NEBULA_ENABLE_SPDK
    if (use_spdk) nebula_spdk_env_fini();
#endif
    bool pass = (ok == count);
    std::cout << (pass ? "[PASS] All tests passed!\n" : "[FAIL] Some tests failed.\n");
    return pass ? 0 : 1;
}
