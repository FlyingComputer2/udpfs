// udpfs wire format (docs/protocol.md): op codes, wire errno and open flags,
// and little-endian readers / writers. No I/O, no configuration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace udpfs::wire
{

enum op : uint8_t
{
    op_open = 0x01,
    op_close = 0x02,
    op_read = 0x03,
    op_write = 0x04,
    op_lseek = 0x05,
    op_fstat = 0x06,
    op_stat = 0x07,
    op_unlink = 0x08,
    op_mkdir = 0x09,
    op_rmdir = 0x0a,
    op_opendir = 0x0b,
    op_readdir = 0x0c,
    op_closedir = 0x0d,
    op_rename = 0x0e,
    op_fsync = 0x0f,
    op_ftruncate = 0x10,
    op_info = 0x11,
    op_crc32 = 0x12,
};
inline constexpr uint8_t k_response_bit = 0x80;

inline constexpr size_t k_request_header = 5;   // op, reqid
inline constexpr size_t k_response_header = 13; // op, reqid, ret, err
// READ / WRITE fixed parts after the header.
inline constexpr size_t k_write_fixed = 4 + 8 + 4;

// Errno, Linux numbering.
inline constexpr uint32_t e_perm = 1, e_noent = 2, e_io = 5, e_badf = 9, e_again = 11, e_nomem = 12,
                          e_acces = 13, e_busy = 16, e_exist = 17, e_xdev = 18, e_notdir = 20,
                          e_isdir = 21, e_inval = 22, e_nfile = 23, e_mfile = 24, e_fbig = 27,
                          e_nospc = 28, e_rofs = 30, e_nametoolong = 36, e_nosys = 38,
                          e_notempty = 39, e_proto = 71, e_msgsize = 90, e_notsup = 95,
                          e_timedout = 110;

// Open flags, Linux numbering.
inline constexpr uint32_t o_rdonly = 0x0, o_wronly = 0x1, o_rdwr = 0x2, o_accmode = 0x3,
                          o_creat = 0x40, o_excl = 0x80, o_trunc = 0x200;
inline constexpr uint32_t o_known = o_accmode | o_creat | o_excl | o_trunc;

inline constexpr uint8_t seek_set = 0, seek_cur = 1, seek_end = 2;

inline constexpr uint8_t type_file = 1, type_dir = 2, type_other = 3;
inline constexpr uint8_t readdir_stat = 0x01;

struct stat_block
{
    uint8_t type = type_other;
    uint32_t mode = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
};
inline constexpr size_t k_stat_block = 1 + 4 + 8 + 8;

struct dir_entry
{
    uint8_t type = type_other;
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string name;
};
inline constexpr size_t k_dir_entry_fixed = 1 + 8 + 8 + 1;

struct info_block
{
    uint32_t max_datagram = 0;
    uint32_t max_path = 0;
    uint32_t max_handles = 0;
    uint32_t reply_cache = 0;
    uint32_t idle_timeout_ms = 0;
    uint32_t crc_max = 0;
    uint32_t open_handles = 0;
};

// Appends little-endian fields.
class writer
{
public:
    explicit writer(std::vector<uint8_t> &out) : out_(out) {}
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void i32(int32_t v) { put(static_cast<uint32_t>(v), 4); }
    void u64(uint64_t v) { put(v, 8); }
    void i64(int64_t v) { put(static_cast<uint64_t>(v), 8); }
    void bytes(const void *p, size_t n)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        out_.insert(out_.end(), b, b + n);
    }
    void str(std::string_view s) { bytes(s.data(), s.size()); }
    void stat(const stat_block &s)
    {
        u8(s.type);
        u32(s.mode);
        u64(s.size);
        i64(s.mtime);
    }

private:
    void put(uint64_t v, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
    std::vector<uint8_t> &out_;
};

// Reads little-endian fields; any read past the end sets failed() and returns 0.
class reader
{
public:
    reader(const uint8_t *p, size_t n) : p_(p), n_(n) {}
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] size_t left() const { return n_ - pos_; }
    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    int32_t i32() { return static_cast<int32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    int64_t i64() { return static_cast<int64_t>(get(8)); }
    // n bytes, or the rest when n is npos.
    std::string_view bytes(size_t n = std::string_view::npos)
    {
        if (n == std::string_view::npos)
        {
            n = left();
        }
        if (n > left())
        {
            failed_ = true;
            return {};
        }
        std::string_view v(reinterpret_cast<const char *>(p_ + pos_), n);
        pos_ += n;
        return v;
    }
    stat_block stat()
    {
        stat_block s;
        s.type = u8();
        s.mode = u32();
        s.size = u64();
        s.mtime = i64();
        return s;
    }

private:
    uint64_t get(int n)
    {
        if (static_cast<size_t>(n) > left())
        {
            failed_ = true;
            pos_ = n_;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i)
        {
            v |= static_cast<uint64_t>(p_[pos_ + i]) << (8 * i);
        }
        pos_ += n;
        return v;
    }
    const uint8_t *p_;
    size_t n_;
    size_t pos_ = 0;
    bool failed_ = false;
};

// CRC-32 (zlib / PNG: IEEE 802.3, reflected). crc32(0, ...) starts a new one;
// pass the previous result to continue.
uint32_t crc32(uint32_t seed, const void *data, size_t n);

}  // namespace udpfs::wire
