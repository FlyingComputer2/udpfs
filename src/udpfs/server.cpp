#include "udpfs/server.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <utility>

#include "udpfs/config.hpp"

namespace udpfs
{
namespace
{

using wire::reader;
using wire::writer;

void put_header(std::vector<uint8_t> *resp, uint8_t op, uint32_t reqid, int32_t ret, uint32_t err)
{
    resp->clear();
    writer w(*resp);
    w.u8(static_cast<uint8_t>(op | wire::k_response_bit));
    w.u32(reqid);
    w.i32(ret);
    w.u32(err);
}

void put_error(std::vector<uint8_t> *resp, uint8_t op, uint32_t reqid, uint32_t wire_err)
{
    put_header(resp, op, reqid, -1, wire_err);
}

void put_errno(std::vector<uint8_t> *resp, uint8_t op, uint32_t reqid, int native)
{
    put_error(resp, op, reqid, config::errno_to_wire(native));
}

wire::stat_block to_block(const struct stat &st)
{
    wire::stat_block b;
    b.type = S_ISREG(st.st_mode) ? wire::type_file : S_ISDIR(st.st_mode) ? wire::type_dir : wire::type_other;
    b.mode = static_cast<uint32_t>(st.st_mode) & 0777u;
    b.size = st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
    b.mtime = static_cast<int64_t>(st.st_mtime);
    return b;
}

// Operations whose retransmission must not run again (docs/protocol.md).
bool cached_op(uint8_t op)
{
    switch (op)
    {
    case wire::op_open:
    case wire::op_close:
    case wire::op_lseek:
    case wire::op_unlink:
    case wire::op_mkdir:
    case wire::op_rmdir:
    case wire::op_opendir:
    case wire::op_readdir:
    case wire::op_closedir:
    case wire::op_rename:
        return true;
    default:
        return false;
    }
}

// Operations that name a handle first after the header.
bool handle_op(uint8_t op)
{
    switch (op)
    {
    case wire::op_close:
    case wire::op_read:
    case wire::op_write:
    case wire::op_lseek:
    case wire::op_fstat:
    case wire::op_readdir:
    case wire::op_closedir:
    case wire::op_fsync:
    case wire::op_ftruncate:
    case wire::op_crc32:
        return true;
    default:
        return false;
    }
}

}  // namespace

server::server(server_config cfg) : cfg_(cfg)
{
    uint32_t seed = cfg_.handle_seed;
    if (0 == seed)
    {
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        seed = static_cast<uint32_t>(t ^ (t >> 32));
    }
    next_handle_ = static_cast<int32_t>(seed % ((1u << 30) - 1)) + 1;
    buf_.resize(std::max<size_t>(cfg_.max_datagram, 4096));
}

server::~server()
{
    reset();
}

void server::reset()
{
    while (!handles_.empty())
    {
        close_handle(handles_.begin()->first);
    }
    cache_.clear();
}

void server::poll(uint64_t now_ms)
{
    if (any_request_ && now_ms - last_request_ms_ >= cfg_.idle_timeout_ms &&
        (!handles_.empty() || !cache_.empty()))
    {
        if (!handles_.empty())
        {
            ++stats_.idle_closes;
        }
        reset();
    }
}

int32_t server::new_handle(handle_t h)
{
    if (handles_.size() >= cfg_.max_handles)
    {
        return -EMFILE;
    }
    while (handles_.count(next_handle_))
    {
        next_handle_ = next_handle_ == INT32_MAX ? 1 : next_handle_ + 1;
    }
    const int32_t id = next_handle_;
    next_handle_ = next_handle_ == INT32_MAX ? 1 : next_handle_ + 1;
    handles_.emplace(id, std::move(h));
    return id;
}

server::handle_t *server::find(int32_t h)
{
    auto it = handles_.find(h);
    return it == handles_.end() ? nullptr : &it->second;
}

void server::close_handle(int32_t h)
{
    auto it = handles_.find(h);
    if (it == handles_.end())
    {
        return;
    }
    if (it->second.dir)
    {
        closedir(static_cast<DIR *>(it->second.dirp));
    }
    else
    {
        ::close(it->second.fd);
    }
    handles_.erase(it);
}

// 0, or a native errno.
int server::path_arg(std::string_view p, std::string *out) const
{
    if (p.empty() || p.front() != '/')
    {
        return EINVAL;
    }
    if (p.size() > cfg_.max_path)
    {
        return ENAMETOOLONG;
    }
    if (p.find('\0') != std::string_view::npos)
    {
        return EINVAL;
    }
    out->assign(p);
    return 0;
}

bool server::handle(const uint8_t *req, size_t n, uint64_t now_ms, std::vector<uint8_t> *resp)
{
    if (n < wire::k_request_header || nullptr == resp)
    {
        ++stats_.malformed;
        return false;
    }
    ++stats_.requests;
    any_request_ = true;
    last_request_ms_ = now_ms;

    reader r(req, n);
    const uint8_t op = r.u8();
    const uint32_t reqid = r.u32();
    if (n > cfg_.max_datagram)
    {
        ++stats_.malformed;
        put_error(resp, op, reqid, wire::e_msgsize);
        return true;
    }

    int32_t key_handle = -1;
    if (handle_op(op) && r.left() >= 4)
    {
        reader peek(req + wire::k_request_header, n - wire::k_request_header);
        key_handle = peek.i32();
    }
    const bool cache = cached_op(op);
    if (cache)
    {
        for (const auto &c : cache_)
        {
            if (c.op == op && c.handle == key_handle && c.reqid == reqid)
            {
                ++stats_.cached;
                *resp = c.resp;
                return true;
            }
        }
    }

    dispatch(op, reqid, r, resp);

    if (resp->size() >= wire::k_response_header)
    {
        reader rr(resp->data() + 5, 4);
        if (rr.i32() < 0)
        {
            reader re(resp->data() + 9, 4);
            const uint32_t e = re.u32();
            if (e == wire::e_proto || e == wire::e_msgsize)
            {
                ++stats_.malformed;
            }
            else
            {
                ++stats_.errors;
            }
        }
    }
    if (cache && cfg_.reply_cache > 0)
    {
        if (cache_.size() >= cfg_.reply_cache)
        {
            cache_.pop_front();
        }
        cache_.push_back({op, key_handle, reqid, *resp});
    }
    return true;
}

void server::dispatch(uint8_t op, uint32_t reqid, wire::reader &r, std::vector<uint8_t> *resp)
{
    auto proto = [&] { put_error(resp, op, reqid, wire::e_proto); };
    auto ok = [&](int32_t ret) { put_header(resp, op, reqid, ret, 0); };
    auto fail = [&](int native) { put_errno(resp, op, reqid, native); };

    switch (op)
    {
    case wire::op_open:
    {
        const uint32_t flags = r.u32();
        const uint32_t mode = r.u32();
        const std::string_view p = r.bytes();
        if (r.failed())
        {
            return proto();
        }
        std::string path;
        if (const int e = path_arg(p, &path))
        {
            return fail(e);
        }
        const int native = config::open_flags_from_wire(flags);
        if (native < 0)
        {
            return fail(EINVAL);
        }
        if (handles_.size() >= cfg_.max_handles)
        {
            return fail(EMFILE);
        }
        const int fd = ::open(path.c_str(), native, static_cast<mode_t>(mode & 0777u));
        if (fd < 0)
        {
            return fail(errno);
        }
        struct stat st {};
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
        {
            ::close(fd);
            return fail(EISDIR);
        }
        handle_t h;
        h.fd = fd;
        h.path = path;
        return ok(new_handle(std::move(h)));
    }
    case wire::op_close:
    case wire::op_closedir:
    {
        const int32_t id = r.i32();
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir != (op == wire::op_closedir))
        {
            return fail(EBADF);
        }
        int rc = 0;
        if (!h->dir && ::close(h->fd) != 0)
        {
            rc = errno;
        }
        if (h->dir)
        {
            closedir(static_cast<DIR *>(h->dirp));
        }
        handles_.erase(id);
        return rc ? fail(rc) : ok(0);
    }
    case wire::op_read:
    {
        const int32_t id = r.i32();
        const uint64_t off = r.u64();
        const uint32_t len = r.u32();
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir)
        {
            return fail(EBADF);
        }
        const size_t room = cfg_.max_datagram - wire::k_response_header;
        const size_t want = std::min<size_t>({len, room, buf_.size()});
        if (off > static_cast<uint64_t>(INT64_MAX) ||
            ::lseek(h->fd, static_cast<off_t>(off), SEEK_SET) < 0)
        {
            return fail(errno ? errno : EINVAL);
        }
        const ssize_t got = ::read(h->fd, buf_.data(), want);
        if (got < 0)
        {
            return fail(errno);
        }
        stats_.bytes_read += static_cast<uint64_t>(got);
        ok(static_cast<int32_t>(got));
        writer(*resp).bytes(buf_.data(), static_cast<size_t>(got));
        return;
    }
    case wire::op_write:
    {
        const int32_t id = r.i32();
        const uint64_t off = r.u64();
        const uint32_t len = r.u32();
        const std::string_view data = r.bytes();
        if (r.failed() || data.size() != len)
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir)
        {
            return fail(EBADF);
        }
        if (off > static_cast<uint64_t>(INT64_MAX) ||
            ::lseek(h->fd, static_cast<off_t>(off), SEEK_SET) < 0)
        {
            return fail(errno ? errno : EINVAL);
        }
        const ssize_t put = ::write(h->fd, data.data(), data.size());
        if (put < 0)
        {
            return fail(errno);
        }
        stats_.bytes_written += static_cast<uint64_t>(put);
        return ok(static_cast<int32_t>(put));
    }
    case wire::op_lseek:
    {
        const int32_t id = r.i32();
        const int64_t off = r.i64();
        const uint8_t whence = r.u8();
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir)
        {
            return fail(EBADF);
        }
        int64_t base = 0;
        if (whence == wire::seek_cur)
        {
            base = h->pos;
        }
        else if (whence == wire::seek_end)
        {
            struct stat st {};
            if (fstat(h->fd, &st) != 0)
            {
                return fail(errno);
            }
            base = static_cast<int64_t>(st.st_size);
        }
        else if (whence != wire::seek_set)
        {
            return fail(EINVAL);
        }
        if ((off > 0 && base > INT64_MAX - off) || base + off < 0)
        {
            return fail(EINVAL);
        }
        h->pos = base + off;
        ok(0);
        writer(*resp).i64(h->pos);
        return;
    }
    case wire::op_fstat:
    case wire::op_stat:
    {
        struct stat st {};
        if (op == wire::op_fstat)
        {
            const int32_t id = r.i32();
            if (r.failed())
            {
                return proto();
            }
            handle_t *h = find(id);
            if (!h || h->dir)
            {
                return fail(EBADF);
            }
            if (fstat(h->fd, &st) != 0)
            {
                return fail(errno);
            }
        }
        else
        {
            std::string path;
            if (const int e = path_arg(r.bytes(), &path))
            {
                return fail(e);
            }
            if (::stat(path.c_str(), &st) != 0)
            {
                // ESP-IDF's VFS cannot stat a mount point (/sdcard, /winject) but
                // can open it as a directory.
                const int e = errno;
                DIR *d = ::opendir(path.c_str());
                if (!d)
                {
                    return fail(e);
                }
                ::closedir(d);
                st = {};
                st.st_mode = S_IFDIR | 0777;
            }
        }
        ok(0);
        writer(*resp).stat(to_block(st));
        return;
    }
    case wire::op_unlink:
    case wire::op_rmdir:
    case wire::op_mkdir:
    case wire::op_opendir:
    {
        uint32_t mode = 0;
        if (op == wire::op_mkdir)
        {
            mode = r.u32();
        }
        const std::string_view p = r.bytes();
        if (r.failed())
        {
            return proto();
        }
        std::string path;
        if (const int e = path_arg(p, &path))
        {
            return fail(e);
        }
        if (op == wire::op_unlink)
        {
            return ::unlink(path.c_str()) == 0 ? ok(0) : fail(errno);
        }
        if (op == wire::op_rmdir)
        {
            return ::rmdir(path.c_str()) == 0 ? ok(0) : fail(errno);
        }
        if (op == wire::op_mkdir)
        {
            return ::mkdir(path.c_str(), static_cast<mode_t>(mode & 0777u)) == 0 ? ok(0) : fail(errno);
        }
        if (handles_.size() >= cfg_.max_handles)
        {
            return fail(EMFILE);
        }
        DIR *d = ::opendir(path.c_str());
        if (!d)
        {
            return fail(errno);
        }
        handle_t h;
        h.dir = true;
        h.dirp = d;
        h.path = path;
        return ok(new_handle(std::move(h)));
    }
    case wire::op_readdir:
    {
        const int32_t id = r.i32();
        const uint8_t rflags = r.u8();
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || !h->dir)
        {
            return fail(EBADF);
        }
        std::vector<uint8_t> entries;
        writer w(entries);
        int32_t count = 0;
        const size_t room = cfg_.max_datagram - wire::k_response_header;
        for (;;)
        {
            if (!h->have_pending)
            {
                errno = 0;
                const struct dirent *de = ::readdir(static_cast<DIR *>(h->dirp));
                if (!de)
                {
                    if (errno != 0 && count == 0)
                    {
                        return fail(errno);
                    }
                    break;
                }
                const std::string_view name(de->d_name);
                if (name == "." || name == ".." || name.empty() || name.size() > 255)
                {
                    continue;
                }
                wire::dir_entry e;
                e.name.assign(name);
                bool need_stat = (rflags & wire::readdir_stat) != 0;
#if defined(DT_DIR) && defined(DT_REG)
                if (de->d_type == DT_DIR)
                {
                    e.type = wire::type_dir;
                }
                else if (de->d_type == DT_REG)
                {
                    e.type = wire::type_file;
                }
                else
                {
                    need_stat = true;
                }
#else
                need_stat = true;
#endif
                if (need_stat)
                {
                    std::string full = h->path;
                    if (full.empty() || full.back() != '/')
                    {
                        full += '/';
                    }
                    full += e.name;
                    struct stat st {};
                    if (::stat(full.c_str(), &st) == 0)
                    {
                        const auto b = to_block(st);
                        e.type = b.type;
                        if (rflags & wire::readdir_stat)
                        {
                            e.size = b.size;
                            e.mtime = b.mtime;
                        }
                    }
                }
                h->pending = std::move(e);
                h->have_pending = true;
            }
            const wire::dir_entry &e = h->pending;
            if (entries.size() + wire::k_dir_entry_fixed + e.name.size() > room)
            {
                break;
            }
            w.u8(e.type);
            w.u64(e.size);
            w.i64(e.mtime);
            w.u8(static_cast<uint8_t>(e.name.size()));
            w.str(e.name);
            ++count;
            h->have_pending = false;
        }
        ok(count);
        writer(*resp).bytes(entries.data(), entries.size());
        return;
    }
    case wire::op_rename:
    {
        const uint16_t from_len = r.u16();
        const std::string_view from = r.bytes(from_len);
        const std::string_view to = r.bytes();
        if (r.failed())
        {
            return proto();
        }
        std::string a;
        std::string b;
        if (const int e = path_arg(from, &a))
        {
            return fail(e);
        }
        if (const int e = path_arg(to, &b))
        {
            return fail(e);
        }
        return ::rename(a.c_str(), b.c_str()) == 0 ? ok(0) : fail(errno);
    }
    case wire::op_fsync:
    case wire::op_ftruncate:
    {
        const int32_t id = r.i32();
        uint64_t len = 0;
        if (op == wire::op_ftruncate)
        {
            len = r.u64();
        }
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir)
        {
            return fail(EBADF);
        }
        if (op == wire::op_fsync)
        {
            return ::fsync(h->fd) == 0 ? ok(0) : fail(errno);
        }
        if (len > static_cast<uint64_t>(INT64_MAX))
        {
            return fail(EINVAL);
        }
        return ::ftruncate(h->fd, static_cast<off_t>(len)) == 0 ? ok(0) : fail(errno);
    }
    case wire::op_info:
    {
        ok(0);
        writer w(*resp);
        w.u32(static_cast<uint32_t>(cfg_.max_datagram));
        w.u32(static_cast<uint32_t>(cfg_.max_path));
        w.u32(static_cast<uint32_t>(cfg_.max_handles));
        w.u32(static_cast<uint32_t>(cfg_.reply_cache));
        w.u32(cfg_.idle_timeout_ms);
        w.u32(cfg_.crc_max);
        w.u32(static_cast<uint32_t>(handles_.size()));
        return;
    }
    case wire::op_crc32:
    {
        const int32_t id = r.i32();
        const uint64_t off = r.u64();
        const uint64_t len = r.u64();
        uint32_t crc = r.u32();
        if (r.failed())
        {
            return proto();
        }
        handle_t *h = find(id);
        if (!h || h->dir)
        {
            return fail(EBADF);
        }
        if (off > static_cast<uint64_t>(INT64_MAX) ||
            ::lseek(h->fd, static_cast<off_t>(off), SEEK_SET) < 0)
        {
            return fail(errno ? errno : EINVAL);
        }
        const uint64_t limit = std::min<uint64_t>(len, cfg_.crc_max);
        uint64_t done = 0;
        while (done < limit)
        {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(limit - done, buf_.size()));
            const ssize_t got = ::read(h->fd, buf_.data(), want);
            if (got < 0)
            {
                return fail(errno);
            }
            if (got == 0)
            {
                break;
            }
            crc = wire::crc32(crc, buf_.data(), static_cast<size_t>(got));
            done += static_cast<uint64_t>(got);
        }
        ok(0);
        writer w(*resp);
        w.u32(crc);
        w.u64(done);
        return;
    }
    case wire::op_list_open:
    {
        std::vector<uint8_t> entries;
        writer w(entries);
        int32_t count = 0;
        const size_t room = cfg_.max_datagram - wire::k_response_header;
        for (const auto &[id, h] : handles_)
        {
            const size_t plen = std::min<size_t>(h.path.size(), 0xffff);
            if (entries.size() + wire::k_open_entry_fixed + plen > room)
            {
                break;
            }
            w.i32(id);
            w.u8(h.dir ? wire::type_dir : wire::type_file);
            w.i64(h.dir ? 0 : h.pos);
            w.u16(static_cast<uint16_t>(plen));
            w.bytes(h.path.data(), plen);
            ++count;
        }
        ok(count);
        writer(*resp).bytes(entries.data(), entries.size());
        return;
    }
    default:
        return put_error(resp, op, reqid, wire::e_nosys);
    }
}

}  // namespace udpfs
