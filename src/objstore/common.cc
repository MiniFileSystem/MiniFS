/* objstore/common.cc - serialization, BE encoding, path validation. */
#include "common.h"

#include <cstring>
#include <ctime>
#include <type_traits>

namespace nebula::objstore {

const char *status_str(Status s)
{
    switch (s) {
    case Status::OK:                       return "OK";
    case Status::ERROR_NOT_FOUND:          return "NOT_FOUND";
    case Status::ERROR_ALREADY_EXISTS:     return "ALREADY_EXISTS";
    case Status::ERROR_PARENT_NOT_FOUND:   return "PARENT_NOT_FOUND";
    case Status::ERROR_OUT_OF_RANGE:       return "OUT_OF_RANGE";
    case Status::ERROR_NO_SPACE:           return "NO_SPACE";
    case Status::ERROR_IO_FAILURE:         return "IO_FAILURE";
    case Status::ERROR_METADATA:           return "METADATA";
    case Status::ERROR_INVALID_ARGUMENT:   return "INVALID_ARGUMENT";
    case Status::ERROR_INTERNAL:           return "INTERNAL";
    }
    return "?";
}

uint64_t now_ns()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/* ----------------------------------------------------------------------
 * Path validation: exactly /<tenant>/<sub>/<ds>/<oid>, all non-empty,
 * no embedded slashes inside a component.
 * ---------------------------------------------------------------------- */
bool validate_object_path(const std::string &p)
{
    if (p.size() < 5) return false;        /* "/a/b/c/d" minimum */
    if (p[0] != '/')  return false;

    int component_count = 0;
    size_t start = 1;
    for (size_t i = 1; i <= p.size(); ++i) {
        bool at_end  = (i == p.size());
        bool is_sep  = !at_end && p[i] == '/';
        if (is_sep || at_end) {
            if (i == start) return false;   /* empty component */
            ++component_count;
            start = i + 1;
        }
    }
    return component_count == 4;
}

/* ----------------------------------------------------------------------
 * Big-endian helpers - all numeric key components are encoded BE so
 * RocksDB's lex-sorted iteration matches numeric order.
 * ---------------------------------------------------------------------- */
void put_be16(std::string &out, uint16_t v)
{
    char b[2];
    b[0] = (char)(v >> 8);
    b[1] = (char)(v);
    out.append(b, 2);
}

void put_be32(std::string &out, uint32_t v)
{
    char b[4];
    b[0] = (char)(v >> 24);
    b[1] = (char)(v >> 16);
    b[2] = (char)(v >> 8);
    b[3] = (char)(v);
    out.append(b, 4);
}

void put_be64(std::string &out, uint64_t v)
{
    char b[8];
    for (int i = 0; i < 8; ++i) b[7 - i] = (char)(v >> (i * 8));
    out.append(b, 8);
}

uint16_t get_be16(const char *p)
{
    return ((uint16_t)(uint8_t)p[0] << 8) | (uint16_t)(uint8_t)p[1];
}

uint32_t get_be32(const char *p)
{
    return ((uint32_t)(uint8_t)p[0] << 24) |
           ((uint32_t)(uint8_t)p[1] << 16) |
           ((uint32_t)(uint8_t)p[2] << 8)  |
            (uint32_t)(uint8_t)p[3];
}

uint64_t get_be64(const char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | (uint64_t)(uint8_t)p[i];
    return v;
}

/* ----------------------------------------------------------------------
 * Tiny fixed-endianness POD encoder for VALUES.  Values are opaque to
 * RocksDB and we always read on the same host that wrote, so host
 * byte order is fine for value bodies.
 * ---------------------------------------------------------------------- */
template <typename T>
static void put_pod(std::string &out, const T &v)
{
    static_assert(std::is_trivially_copyable<T>::value, "POD only");
    out.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
static bool get_pod(const std::string &in, size_t &pos, T *out)
{
    if (pos + sizeof(T) > in.size()) return false;
    std::memcpy(out, in.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

/* ---- ObjectAttrs ----------------------------------------------------- */
std::string encode_attrs(const ObjectAttrs &a)
{
    std::string out;
    out.reserve(sizeof(ObjectAttrs));
    put_pod(out, a.object_id);
    put_pod(out, a.size);
    put_pod(out, a.ctime_ns);
    put_pod(out, a.mtime_ns);
    put_pod(out, a.version);
    put_pod(out, a.num_extents);
    return out;
}

bool decode_attrs(const std::string &buf, ObjectAttrs *out)
{
    size_t pos = 0;
    if (!get_pod(buf, pos, &out->object_id))   return false;
    if (!get_pod(buf, pos, &out->size))        return false;
    if (!get_pod(buf, pos, &out->ctime_ns))    return false;
    if (!get_pod(buf, pos, &out->mtime_ns))    return false;
    if (!get_pod(buf, pos, &out->version))     return false;
    if (!get_pod(buf, pos, &out->num_extents)) return false;
    return pos == buf.size();
}

/* ---- ExtentEntry ----------------------------------------------------- */
std::string encode_extent(const ExtentEntry &e)
{
    std::string out;
    out.reserve(sizeof(ExtentEntry));
    put_pod(out, e.chunk_id);
    put_pod(out, e.chunk_idx);
    put_pod(out, e.reserved);
    put_pod(out, e.offset_in_chunk);
    put_pod(out, e.length);
    put_pod(out, e.checksum);
    return out;
}

bool decode_extent(const std::string &buf, ExtentEntry *out)
{
    size_t pos = 0;
    if (!get_pod(buf, pos, &out->chunk_id))        return false;
    if (!get_pod(buf, pos, &out->chunk_idx))       return false;
    if (!get_pod(buf, pos, &out->reserved))        return false;
    if (!get_pod(buf, pos, &out->offset_in_chunk)) return false;
    if (!get_pod(buf, pos, &out->length))          return false;
    if (!get_pod(buf, pos, &out->checksum))        return false;
    return pos == buf.size();
}

/* ---- Chunk ------------------------------------------------------------ */
std::string encode_chunk(const Chunk &c)
{
    std::string out;
    out.reserve(32);
    put_pod(out, c.chunk_id);
    put_pod(out, c.lba_start);
    put_pod(out, c.write_offset);
    put_pod(out, c.next_idx);
    uint8_t sealed = c.sealed ? 1 : 0;
    put_pod(out, sealed);
    return out;
}

bool decode_chunk(const std::string &buf, Chunk *out)
{
    size_t pos = 0;
    if (!get_pod(buf, pos, &out->chunk_id))     return false;
    if (!get_pod(buf, pos, &out->lba_start))    return false;
    if (!get_pod(buf, pos, &out->write_offset)) return false;
    if (!get_pod(buf, pos, &out->next_idx))     return false;
    uint8_t sealed = 0;
    if (!get_pod(buf, pos, &sealed))            return false;
    out->sealed = sealed != 0;
    return pos == buf.size();
}

/* ---- ChunkRevEntry --------------------------------------------------- */
std::string encode_chunk_rev(const ChunkRevEntry &e)
{
    std::string out;
    out.reserve(sizeof(ChunkRevEntry));
    put_pod(out, e.oid);
    put_pod(out, e.logical_offset);
    put_pod(out, e.length);
    put_pod(out, e.reserved);
    return out;
}

bool decode_chunk_rev(const std::string &buf, ChunkRevEntry *out)
{
    size_t pos = 0;
    if (!get_pod(buf, pos, &out->oid))            return false;
    if (!get_pod(buf, pos, &out->logical_offset)) return false;
    if (!get_pod(buf, pos, &out->length))         return false;
    if (!get_pod(buf, pos, &out->reserved))       return false;
    return pos == buf.size();
}

} /* namespace nebula::objstore */
