/*
 * nebula_rocksdb_fs.cc - RocksDB FileSystem adapter implementation.
 */
#include "nebula_rocksdb_fs.h"

#include <rocksdb/env.h>
#include <rocksdb/status.h>

#include <cerrno>
#include <cstring>
#include <algorithm>

extern "C" {
#include "nebula/nebula_fs.h"
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
}

namespace nebula {

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static rocksdb::IOStatus io_err(int rc, const std::string &ctx)
{
    if (rc == 0) return rocksdb::IOStatus::OK();
    if (rc == -ENOENT) return rocksdb::IOStatus::NotFound(ctx);
    if (rc == -EEXIST) return rocksdb::IOStatus::InvalidArgument(ctx + ": exists");
    if (rc == -ENOSPC) return rocksdb::IOStatus::NoSpace(ctx);
    return rocksdb::IOStatus::IOError(ctx, std::strerror(-rc));
}

/* -----------------------------------------------------------------------
 * C2: NebulaSequentialFile
 * ----------------------------------------------------------------------- */

NebulaSequentialFile::NebulaSequentialFile(nebula_fh_t *fh, uint64_t sz)
    : fh_(fh), pos_(0), size_(sz) {}

NebulaSequentialFile::~NebulaSequentialFile()
{
    if (fh_) nebula_fs_close(fh_);
}

rocksdb::IOStatus NebulaSequentialFile::Read(size_t n,
                                              const rocksdb::IOOptions &,
                                              rocksdb::Slice *result,
                                              char *scratch,
                                              rocksdb::IODebugContext *)
{
    if (pos_ >= size_) {
        *result = rocksdb::Slice(scratch, 0);
        return rocksdb::IOStatus::OK();
    }
    size_t to_read = std::min(n, (size_t)(size_ - pos_));
    ssize_t got = nebula_fs_read(fh_, pos_, scratch, to_read);
    if (got < 0)
        return rocksdb::IOStatus::IOError("NebulaSequentialFile::Read",
                                          std::strerror((int)-got));
    pos_ += (uint64_t)got;
    *result = rocksdb::Slice(scratch, (size_t)got);
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaSequentialFile::Skip(uint64_t n)
{
    pos_ = std::min(pos_ + n, size_);
    return rocksdb::IOStatus::OK();
}

/* -----------------------------------------------------------------------
 * C3: NebulaRandomAccessFile
 * ----------------------------------------------------------------------- */

NebulaRandomAccessFile::NebulaRandomAccessFile(nebula_fh_t *fh) : fh_(fh) {}

NebulaRandomAccessFile::~NebulaRandomAccessFile()
{
    if (fh_) nebula_fs_close(fh_);
}

rocksdb::IOStatus NebulaRandomAccessFile::Read(uint64_t offset, size_t n,
                                                const rocksdb::IOOptions &,
                                                rocksdb::Slice *result,
                                                char *scratch,
                                                rocksdb::IODebugContext *) const
{
    uint64_t sz = nebula_fs_file_size(fh_);
    if (offset >= sz) {
        *result = rocksdb::Slice(scratch, 0);
        return rocksdb::IOStatus::OK();
    }
    size_t to_read = std::min(n, (size_t)(sz - offset));
    ssize_t got = nebula_fs_read(fh_, offset, scratch, to_read);
    if (got < 0)
        return rocksdb::IOStatus::IOError("NebulaRandomAccessFile::Read",
                                          std::strerror((int)-got));
    *result = rocksdb::Slice(scratch, (size_t)got);
    return rocksdb::IOStatus::OK();
}

/* -----------------------------------------------------------------------
 * C4: NebulaWritableFile
 * ----------------------------------------------------------------------- */

NebulaWritableFile::NebulaWritableFile(nebula_fh_t *fh, uint64_t initial_size)
    : fh_(fh), pos_(initial_size), closed_(false) {}

NebulaWritableFile::~NebulaWritableFile()
{
    if (!closed_ && fh_) nebula_fs_close(fh_);
}

rocksdb::IOStatus NebulaWritableFile::Append(const rocksdb::Slice &data,
                                              const rocksdb::IOOptions &,
                                              rocksdb::IODebugContext *)
{
    if (closed_) return rocksdb::IOStatus::IOError("file already closed");
    ssize_t n = nebula_fs_write(fh_, pos_, data.data(), data.size());
    if (n < 0)
        return rocksdb::IOStatus::IOError("NebulaWritableFile::Append",
                                          std::strerror((int)-n));
    pos_ += (uint64_t)n;
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaWritableFile::Append(
    const rocksdb::Slice &data, const rocksdb::IOOptions &opts,
    const rocksdb::DataVerificationInfo &, rocksdb::IODebugContext *dbg)
{
    return Append(data, opts, dbg);
}

rocksdb::IOStatus NebulaWritableFile::Flush(const rocksdb::IOOptions &,
                                             rocksdb::IODebugContext *)
{
    return rocksdb::IOStatus::OK();   /* Nebula writes are synchronous */
}

rocksdb::IOStatus NebulaWritableFile::Sync(const rocksdb::IOOptions &,
                                            rocksdb::IODebugContext *)
{
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaWritableFile::Close(const rocksdb::IOOptions &,
                                             rocksdb::IODebugContext *)
{
    if (!closed_ && fh_) {
        int rc = nebula_fs_close(fh_);
        fh_     = nullptr;
        closed_ = true;
        return io_err(rc, "NebulaWritableFile::Close");
    }
    return rocksdb::IOStatus::OK();
}

uint64_t NebulaWritableFile::GetFileSize(const rocksdb::IOOptions &,
                                          rocksdb::IODebugContext *)
{
    return pos_;
}

/* -----------------------------------------------------------------------
 * C5: NebulaFileSystem — helpers
 * ----------------------------------------------------------------------- */

std::string NebulaFileSystem::basename(const std::string &path)
{
    /* Strip everything up to and including the last '/' */
    size_t pos = path.rfind('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/* -----------------------------------------------------------------------
 * C5: NebulaFileSystem — lifecycle
 * ----------------------------------------------------------------------- */

NebulaFileSystem::NebulaFileSystem(const std::string &device_path)
    : fs_(nullptr), device_path_(device_path)
{
    int rc = nebula_fs_mount(device_path.c_str(), &fs_);
    if (rc != 0) {
        throw std::runtime_error("NebulaFileSystem: mount failed on '" +
                                 device_path + "': " + std::strerror(-rc));
    }
}

NebulaFileSystem::NebulaFileSystem(struct nebula_io *io)
    : fs_(nullptr), device_path_("(spdk)")
{
    int rc = nebula_fs_mount_io(io, &fs_);
    if (rc != 0) {
        throw std::runtime_error(
            std::string("NebulaFileSystem: mount_io failed: ") +
            std::strerror(-rc));
    }
}

NebulaFileSystem::~NebulaFileSystem()
{
    if (fs_) nebula_fs_unmount(fs_);
}

/* -----------------------------------------------------------------------
 * C5: File open/create
 * ----------------------------------------------------------------------- */

rocksdb::IOStatus NebulaFileSystem::NewSequentialFile(
    const std::string &fname, const rocksdb::FileOptions &,
    std::unique_ptr<rocksdb::FSSequentialFile> *result,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::string name = basename(fname);

    nebula_fh_t *fh = nullptr;
    int rc = nebula_fs_open(fs_, name.c_str(), NEBULA_FS_O_RDONLY, &fh);
    if (rc != 0) return io_err(rc, "NewSequentialFile:" + name);

    uint64_t sz = nebula_fs_file_size(fh);
    result->reset(new NebulaSequentialFile(fh, sz));
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::NewRandomAccessFile(
    const std::string &fname, const rocksdb::FileOptions &,
    std::unique_ptr<rocksdb::FSRandomAccessFile> *result,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::string name = basename(fname);

    nebula_fh_t *fh = nullptr;
    int rc = nebula_fs_open(fs_, name.c_str(), NEBULA_FS_O_RDONLY, &fh);
    if (rc != 0) return io_err(rc, "NewRandomAccessFile:" + name);

    result->reset(new NebulaRandomAccessFile(fh));
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::NewWritableFile(
    const std::string &fname, const rocksdb::FileOptions &,
    std::unique_ptr<rocksdb::FSWritableFile> *result,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::string name = basename(fname);

    /* Delete existing file first (RocksDB expects create-or-truncate) */
    nebula_fs_delete(fs_, name.c_str());   /* ignore ENOENT */

    nebula_fh_t *fh = nullptr;
    int rc = nebula_fs_create(fs_, name.c_str(), 0644, &fh);
    if (rc != 0) return io_err(rc, "NewWritableFile:" + name);

    result->reset(new NebulaWritableFile(fh, 0));
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::ReopenWritableFile(
    const std::string &fname, const rocksdb::FileOptions &,
    std::unique_ptr<rocksdb::FSWritableFile> *result,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::string name = basename(fname);

    nebula_fh_t *fh = nullptr;
    int rc = nebula_fs_open(fs_, name.c_str(), NEBULA_FS_O_RDWR, &fh);
    if (rc != 0) return io_err(rc, "ReopenWritableFile:" + name);

    uint64_t sz = nebula_fs_file_size(fh);
    result->reset(new NebulaWritableFile(fh, sz));
    return rocksdb::IOStatus::OK();
}

/* -----------------------------------------------------------------------
 * C5: Directory / metadata
 * ----------------------------------------------------------------------- */

/* Minimal FSDirectory implementation (no-op sync) */
class NebulaDirectory final : public rocksdb::FSDirectory {
public:
    rocksdb::IOStatus Fsync(const rocksdb::IOOptions &,
                            rocksdb::IODebugContext *) override {
        return rocksdb::IOStatus::OK();
    }
};

rocksdb::IOStatus NebulaFileSystem::NewDirectory(
    const std::string &, const rocksdb::IOOptions &,
    std::unique_ptr<rocksdb::FSDirectory> *result,
    rocksdb::IODebugContext *)
{
    result->reset(new NebulaDirectory());
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::FileExists(
    const std::string &fname, const rocksdb::IOOptions &,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t inum = 0;
    std::string name = basename(fname);
    int rc = nebula_fs_lookup(fs_, name.c_str(), &inum);
    if (rc == 0) return rocksdb::IOStatus::OK();
    return rocksdb::IOStatus::NotFound(fname);
}

struct ListCtx {
    std::vector<std::string> *out;
};

static int list_cb(const struct nebula_fs_dirent *de, void *ud)
{
    /* Skip the root directory entry and any dir-type entries */
    if (de->flags & NEBULA_DIR_FLAG_DIR) return 0;
    auto *ctx = static_cast<ListCtx *>(ud);
    ctx->out->emplace_back(de->name);
    return 0;
}

rocksdb::IOStatus NebulaFileSystem::GetChildren(
    const std::string &, const rocksdb::IOOptions &,
    std::vector<std::string> *result,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    result->clear();
    ListCtx ctx{ result };
    int rc = nebula_fs_readdir(fs_, list_cb, &ctx);
    return io_err(rc, "GetChildren");
}

rocksdb::IOStatus NebulaFileSystem::DeleteFile(
    const std::string &fname, const rocksdb::IOOptions &,
    rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    int rc = nebula_fs_delete(fs_, basename(fname).c_str());
    return io_err(rc, "DeleteFile:" + fname);
}

rocksdb::IOStatus NebulaFileSystem::GetFileSize(
    const std::string &fname, const rocksdb::IOOptions &,
    uint64_t *file_size, rocksdb::IODebugContext *)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::string name = basename(fname);

    nebula_fh_t *fh = nullptr;
    int rc = nebula_fs_open(fs_, name.c_str(), NEBULA_FS_O_RDONLY, &fh);
    if (rc != 0) return io_err(rc, "GetFileSize:" + name);

    *file_size = nebula_fs_file_size(fh);
    nebula_fs_close(fh);
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::GetFileModificationTime(
    const std::string &fname, const rocksdb::IOOptions &,
    uint64_t *file_mtime, rocksdb::IODebugContext *)
{
    /* Nebula stores mtime_ns in inode; return seconds since epoch */
    std::lock_guard<std::mutex> lk(mu_);
    *file_mtime = (uint64_t)time(nullptr);   /* simplified: no per-file mtime API yet */
    (void)fname;
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::RenameFile(
    const std::string &src, const std::string &target,
    const rocksdb::IOOptions &, rocksdb::IODebugContext *)
{
    /* Rename = lookup src inode, remove src entry, add target entry */
    std::lock_guard<std::mutex> lk(mu_);
    std::string sname = basename(src);
    std::string tname = basename(target);

    uint64_t inum = 0;
    int rc = nebula_fs_lookup(fs_, sname.c_str(), &inum);
    if (rc != 0) return io_err(rc, "RenameFile:lookup:" + sname);

    /* Remove old entry then add new one pointing to same inode.
     * nebula_dir_add / nebula_dir_remove are internal; use public delete+
     * reopen trick: read data, recreate under new name. */
    nebula_fh_t *rfh = nullptr;
    rc = nebula_fs_open(fs_, sname.c_str(), NEBULA_FS_O_RDONLY, &rfh);
    if (rc != 0) return io_err(rc, "RenameFile:open:" + sname);

    uint64_t sz = nebula_fs_file_size(rfh);
    std::vector<char> buf(sz);
    if (sz > 0) {
        ssize_t got = nebula_fs_read(rfh, 0, buf.data(), sz);
        if (got < 0) {
            nebula_fs_close(rfh);
            return rocksdb::IOStatus::IOError("RenameFile:read", std::strerror((int)-got));
        }
    }
    nebula_fs_close(rfh);

    /* Delete old name */
    rc = nebula_fs_delete(fs_, sname.c_str());
    if (rc != 0) return io_err(rc, "RenameFile:delete:" + sname);

    /* Remove target if it already exists (POSIX rename replaces atomically) */
    nebula_fs_delete(fs_, tname.c_str());   /* ignore ENOENT */

    /* Create under new name */
    nebula_fh_t *wfh = nullptr;
    rc = nebula_fs_create(fs_, tname.c_str(), 0644, &wfh);
    if (rc != 0) return io_err(rc, "RenameFile:create:" + tname);

    if (sz > 0) {
        ssize_t written = nebula_fs_write(wfh, 0, buf.data(), sz);
        if (written < 0) {
            nebula_fs_close(wfh);
            return rocksdb::IOStatus::IOError("RenameFile:write", std::strerror((int)-written));
        }
    }
    nebula_fs_close(wfh);
    return rocksdb::IOStatus::OK();
}

/* -----------------------------------------------------------------------
 * C5: Dir management (flat namespace — dirs are no-ops)
 * ----------------------------------------------------------------------- */

rocksdb::IOStatus NebulaFileSystem::CreateDir(
    const std::string &, const rocksdb::IOOptions &, rocksdb::IODebugContext *)
{
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::CreateDirIfMissing(
    const std::string &, const rocksdb::IOOptions &, rocksdb::IODebugContext *)
{
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::DeleteDir(
    const std::string &, const rocksdb::IOOptions &, rocksdb::IODebugContext *)
{
    return rocksdb::IOStatus::OK();
}

/* -----------------------------------------------------------------------
 * C5: Locking (single-process — no-op)
 * ----------------------------------------------------------------------- */

rocksdb::IOStatus NebulaFileSystem::LockFile(
    const std::string &, const rocksdb::IOOptions &,
    rocksdb::FileLock **lock, rocksdb::IODebugContext *)
{
    *lock = new rocksdb::FileLock();
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::UnlockFile(
    rocksdb::FileLock *lock, const rocksdb::IOOptions &,
    rocksdb::IODebugContext *)
{
    delete lock;
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::GetTestDirectory(
    const rocksdb::IOOptions &, std::string *path, rocksdb::IODebugContext *)
{
    *path = "/";
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::GetAbsolutePath(
    const std::string &db_path, const rocksdb::IOOptions &,
    std::string *output_path, rocksdb::IODebugContext *)
{
    *output_path = db_path.empty() ? "/" : db_path;
    return rocksdb::IOStatus::OK();
}

rocksdb::IOStatus NebulaFileSystem::IsDirectory(
    const std::string &, const rocksdb::IOOptions &,
    bool *is_dir, rocksdb::IODebugContext *)
{
    /* Nebula has a flat namespace — nothing is a sub-directory */
    *is_dir = false;
    return rocksdb::IOStatus::OK();
}

/* Minimal logger that discards all messages (avoids null-ptr issues) */
class NebulaDevNullLogger : public rocksdb::Logger {
public:
    explicit NebulaDevNullLogger()
        : rocksdb::Logger(rocksdb::InfoLogLevel::WARN_LEVEL) {}
    void Logv(const char *, va_list) override {}
};

rocksdb::IOStatus NebulaFileSystem::NewLogger(
    const std::string &, const rocksdb::IOOptions &,
    std::shared_ptr<rocksdb::Logger> *result, rocksdb::IODebugContext *)
{
    *result = std::make_shared<NebulaDevNullLogger>();
    return rocksdb::IOStatus::OK();
}

} /* namespace nebula */
