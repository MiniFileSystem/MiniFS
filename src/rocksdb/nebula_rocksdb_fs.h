/*
 * nebula_rocksdb_fs.h - RocksDB FileSystem adapter for MiniFS (Nebula).
 *
 * Implements the RocksDB 8.x FileSystem plugin interface backed by the
 * Nebula FS public API (nebula_fs.h).
 *
 * Usage:
 *   auto fs = std::make_shared<nebula::NebulaFileSystem>("/tmp/test.img");
 *   rocksdb::Options opts;
 *   opts.env = rocksdb::NewCompositeEnv(fs);
 *   rocksdb::DB *db;
 *   rocksdb::DB::Open(opts, "/nebuladb", &db);
 */
#pragma once

#include <rocksdb/file_system.h>
#include <rocksdb/status.h>

#include <memory>
#include <string>
#include <mutex>

extern "C" {
#include "nebula/nebula_fs.h"
}

namespace nebula {

/* -----------------------------------------------------------------------
 * C2: NebulaSequentialFile
 * Wraps nebula_fh_t for sequential reads (cursor advances automatically).
 * ----------------------------------------------------------------------- */
class NebulaSequentialFile final : public rocksdb::FSSequentialFile {
public:
    NebulaSequentialFile(nebula_fh_t *fh, uint64_t file_size);
    ~NebulaSequentialFile() override;

    rocksdb::IOStatus Read(size_t n, const rocksdb::IOOptions &opts,
                           rocksdb::Slice *result, char *scratch,
                           rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus Skip(uint64_t n) override;

private:
    nebula_fh_t *fh_;
    uint64_t     pos_;
    uint64_t     size_;
};

/* -----------------------------------------------------------------------
 * C3: NebulaRandomAccessFile
 * Wraps nebula_fh_t for positional reads (no cursor state).
 * ----------------------------------------------------------------------- */
class NebulaRandomAccessFile final : public rocksdb::FSRandomAccessFile {
public:
    explicit NebulaRandomAccessFile(nebula_fh_t *fh);
    ~NebulaRandomAccessFile() override;

    rocksdb::IOStatus Read(uint64_t offset, size_t n,
                           const rocksdb::IOOptions &opts,
                           rocksdb::Slice *result, char *scratch,
                           rocksdb::IODebugContext *dbg) const override;

private:
    nebula_fh_t *fh_;
};

/* -----------------------------------------------------------------------
 * C4: NebulaWritableFile
 * Wraps nebula_fh_t for sequential appends / positional writes.
 * ----------------------------------------------------------------------- */
class NebulaWritableFile final : public rocksdb::FSWritableFile {
public:
    NebulaWritableFile(nebula_fh_t *fh, uint64_t initial_size);
    ~NebulaWritableFile() override;

    rocksdb::IOStatus Append(const rocksdb::Slice &data,
                             const rocksdb::IOOptions &opts,
                             rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus Append(const rocksdb::Slice &data,
                             const rocksdb::IOOptions &opts,
                             const rocksdb::DataVerificationInfo &,
                             rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus Flush(const rocksdb::IOOptions &opts,
                            rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus Sync(const rocksdb::IOOptions &opts,
                           rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus Close(const rocksdb::IOOptions &opts,
                            rocksdb::IODebugContext *dbg) override;

    uint64_t GetFileSize(const rocksdb::IOOptions &opts,
                         rocksdb::IODebugContext *dbg) override;

private:
    nebula_fh_t *fh_;
    uint64_t     pos_;     /* append cursor */
    bool         closed_;
};

/* -----------------------------------------------------------------------
 * C5: NebulaFileSystem
 * Main FileSystem implementation — creates / opens / lists / deletes files.
 * ----------------------------------------------------------------------- */
class NebulaFileSystem final : public rocksdb::FileSystem {
public:
    /*
     * device_path: path to a formatted Nebula image file or block device.
     * The device must already have been formatted with nebula_format.
     */
    explicit NebulaFileSystem(const std::string &device_path);
    ~NebulaFileSystem() override;

    static const char *kClassName() { return "NebulaFileSystem"; }
    const char *Name() const override { return kClassName(); }

    /* --- File open/create --- */
    rocksdb::IOStatus NewSequentialFile(
        const std::string &fname, const rocksdb::FileOptions &opts,
        std::unique_ptr<rocksdb::FSSequentialFile> *result,
        rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus NewRandomAccessFile(
        const std::string &fname, const rocksdb::FileOptions &opts,
        std::unique_ptr<rocksdb::FSRandomAccessFile> *result,
        rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus NewWritableFile(
        const std::string &fname, const rocksdb::FileOptions &opts,
        std::unique_ptr<rocksdb::FSWritableFile> *result,
        rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus ReopenWritableFile(
        const std::string &fname, const rocksdb::FileOptions &opts,
        std::unique_ptr<rocksdb::FSWritableFile> *result,
        rocksdb::IODebugContext *dbg) override;

    /* --- Directory / metadata --- */
    rocksdb::IOStatus NewDirectory(
        const std::string &name, const rocksdb::IOOptions &opts,
        std::unique_ptr<rocksdb::FSDirectory> *result,
        rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus FileExists(const std::string &fname,
                                 const rocksdb::IOOptions &opts,
                                 rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus GetChildren(const std::string &dir,
                                  const rocksdb::IOOptions &opts,
                                  std::vector<std::string> *result,
                                  rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus DeleteFile(const std::string &fname,
                                 const rocksdb::IOOptions &opts,
                                 rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus GetFileSize(const std::string &fname,
                                  const rocksdb::IOOptions &opts,
                                  uint64_t *file_size,
                                  rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus GetFileModificationTime(
        const std::string &fname, const rocksdb::IOOptions &opts,
        uint64_t *file_mtime, rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus RenameFile(const std::string &src,
                                 const std::string &target,
                                 const rocksdb::IOOptions &opts,
                                 rocksdb::IODebugContext *dbg) override;

    /* --- Dir management (no-ops for flat namespace) --- */
    rocksdb::IOStatus CreateDir(const std::string &dirname,
                                const rocksdb::IOOptions &opts,
                                rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus CreateDirIfMissing(const std::string &dirname,
                                         const rocksdb::IOOptions &opts,
                                         rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus DeleteDir(const std::string &dirname,
                                const rocksdb::IOOptions &opts,
                                rocksdb::IODebugContext *dbg) override;

    /* --- Locking (no-op for single-process) --- */
    rocksdb::IOStatus LockFile(const std::string &fname,
                               const rocksdb::IOOptions &opts,
                               rocksdb::FileLock **lock,
                               rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus UnlockFile(rocksdb::FileLock *lock,
                                 const rocksdb::IOOptions &opts,
                                 rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus GetTestDirectory(const rocksdb::IOOptions &opts,
                                       std::string *path,
                                       rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus GetAbsolutePath(const std::string &db_path,
                                      const rocksdb::IOOptions &opts,
                                      std::string *output_path,
                                      rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus IsDirectory(const std::string &path,
                                  const rocksdb::IOOptions &opts,
                                  bool *is_dir,
                                  rocksdb::IODebugContext *dbg) override;

    rocksdb::IOStatus NewLogger(const std::string &fname,
                                const rocksdb::IOOptions &opts,
                                std::shared_ptr<rocksdb::Logger> *result,
                                rocksdb::IODebugContext *dbg) override;

private:
    /* Strip any leading path prefix — Nebula uses a flat namespace */
    static std::string basename(const std::string &path);

    nebula_fs_t  *fs_;          /* mounted Nebula device */
    std::string   device_path_;
    mutable std::mutex mu_;     /* serialise all calls */
};

} /* namespace nebula */
