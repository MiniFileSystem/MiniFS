/*
 * nebula_rocksdb_test.cc - C7: Smoke test for the Nebula RocksDB adapter.
 *
 * Usage:
 *   nebula_rocksdb_test <device_image>
 *
 * What it does:
 *   1. Mounts the Nebula device via NebulaFileSystem.
 *   2. Opens a RocksDB database on top of it.
 *   3. Writes 10 key-value pairs.
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

static void die(const std::string &msg, const rocksdb::Status &s)
{
    std::cerr << "[FAIL] " << msg << ": " << s.ToString() << "\n";
    std::exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "Usage: nebula_rocksdb_test <device_image>\n";
        return 1;
    }
    const std::string device = argv[1];

    /* --- Mount Nebula device via our FileSystem adapter --- */
    std::shared_ptr<rocksdb::FileSystem> nebula_fs;
    try {
        nebula_fs = std::make_shared<nebula::NebulaFileSystem>(device);
    } catch (const std::exception &e) {
        std::cerr << "[FAIL] Mount failed: " << e.what() << "\n";
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

    /* --- Write 10 key-value pairs --- */
    rocksdb::WriteOptions wo;
    for (int i = 0; i < 10; i++) {
        std::string key = "key_" + std::to_string(i);
        std::string val = "value_" + std::to_string(i * i);
        s = db->Put(wo, key, val);
        if (!s.ok()) die("Put " + key, s);
    }
    std::cout << "[OK] Wrote 10 key-value pairs\n";

    /* --- Read back and verify --- */
    rocksdb::ReadOptions ro;
    int ok = 0;
    for (int i = 0; i < 10; i++) {
        std::string key = "key_" + std::to_string(i);
        std::string expected = "value_" + std::to_string(i * i);
        std::string got;
        s = db->Get(ro, key, &got);
        if (!s.ok()) { std::cerr << "[FAIL] Get " << key << ": " << s.ToString() << "\n"; continue; }
        if (got != expected) {
            std::cerr << "[FAIL] " << key << " = '" << got << "' expected '" << expected << "'\n";
        } else {
            ok++;
        }
    }
    std::cout << "[OK] Read back: " << ok << "/10 correct\n";

    /* --- Close --- */
    db.reset();
    std::cout << (ok == 10 ? "[PASS] All tests passed!\n" : "[FAIL] Some tests failed.\n");
    return ok == 10 ? 0 : 1;
}
