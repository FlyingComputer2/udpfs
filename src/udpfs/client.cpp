#include "udpfs/client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <deque>
#include <map>
#include <random>

#include "udpfs/config.hpp"

namespace udpfs
{
namespace
{

using wire::reader;
using wire::writer;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t random_u32()
{
    std::random_device rd;
    return static_cast<uint32_t>(rd()) ^ static_cast<uint32_t>(now_ms());
}

// A CRC32 request reads this much at most, so the server answers well within
// the (longer) CRC retransmit time even on a slow card.
constexpr uint64_t k_crc_piece = 256 * 1024;
constexpr int k_crc_retransmit_ms = 3000;

}  // namespace

client::client(transport &t, client_config cfg)
    : t_(t), cfg_(cfg), next_reqid_(cfg.reqid_seed ? cfg.reqid_seed : random_u32())
{
    if (cfg_.window == 0)
    {
        cfg_.window = 1;
    }
    rx_.resize(std::max<size_t>(cfg_.max_datagram, 2048) + 64);
}

std::vector<uint8_t> client::begin(uint8_t op)
{
    std::vector<uint8_t> b;
    b.reserve(64);
    writer w(b);
    w.u8(op);
    w.u32(next_reqid_++);
    return b;
}

bool client::parse(const std::vector<uint8_t> &bytes, response *r)
{
    if (bytes.size() < wire::k_response_header)
    {
        return false;
    }
    reader rd(bytes.data() + 5, bytes.size() - 5);
    r->ret = rd.i32();
    const uint32_t e = rd.u32();
    r->err = r->ret < 0 ? (e ? config::errno_from_wire(e) : EIO) : 0;
    r->payload = bytes.data() + wire::k_response_header;
    r->payload_n = bytes.size() - wire::k_response_header;
    return true;
}

int client::pipeline(const std::function<bool(request *)> &next,
                     const std::function<int(const request &, const response &)> &done, size_t window)
{
    return pipeline_impl(next, done, window, cfg_.retransmit_ms);
}

int client::pipeline_impl(const std::function<bool(request *)> &next,
                          const std::function<int(const request &, const response &)> &done,
                          size_t window, int retransmit_ms)
{
    struct slot
    {
        request req;
        uint8_t op;
        uint32_t reqid;
        int64_t sent;
        int tries;
    };
    std::deque<slot> inflight;
    bool more = true;
    auto send = [this](slot &s) {
        ++stats_.sends;
        (void)t_.send(s.req.bytes.data(), s.req.bytes.size());
        s.sent = now_ms();
        ++s.tries;
    };
    for (;;)
    {
        while (more && inflight.size() < window)
        {
            request r;
            if (!next(&r))
            {
                more = false;
                break;
            }
            if (r.bytes.size() < wire::k_request_header)
            {
                return EINVAL;
            }
            if (r.bytes.size() > cfg_.max_datagram)
            {
                return EMSGSIZE;
            }
            reader rd(r.bytes.data(), r.bytes.size());
            slot s {std::move(r), rd.u8(), rd.u32(), 0, 0};
            ++stats_.requests;
            inflight.push_back(std::move(s));
            send(inflight.back());
        }
        if (inflight.empty())
        {
            return 0;
        }
        int64_t wait = INT64_MAX;
        const int64_t now = now_ms();
        for (const auto &s : inflight)
        {
            wait = std::min(wait, s.sent + retransmit_ms - now);
        }
        const int n = t_.recv(rx_.data(), rx_.size(), static_cast<int>(std::max<int64_t>(wait, 0)));
        if (n > 0)
        {
            last_resp_.assign(rx_.begin(), rx_.begin() + n);
            reader rd(last_resp_.data(), last_resp_.size());
            const uint8_t op = rd.u8();
            const uint32_t reqid = rd.u32();
            response resp;
            auto it = std::find_if(inflight.begin(), inflight.end(), [&](const slot &s) {
                return s.reqid == reqid && (s.op | wire::k_response_bit) == op;
            });
            if (rd.failed() || it == inflight.end() || !parse(last_resp_, &resp))
            {
                ++stats_.stray;
            }
            else
            {
                slot s = std::move(*it);
                inflight.erase(it);
                if (const int e = done(s.req, resp))
                {
                    return e;
                }
            }
        }
        else if (n < 0)
        {
            return EIO;
        }
        // Also after a response: a stream of them must not hold off a resend.
        const int64_t t = now_ms();
        for (auto &s : inflight)
        {
            if (t - s.sent >= retransmit_ms)
            {
                if (s.tries >= cfg_.tries)
                {
                    return ETIMEDOUT;
                }
                ++stats_.retransmits;
                send(s);
            }
        }
    }
}

int client::call_impl(std::vector<uint8_t> req, std::vector<uint8_t> *resp_bytes, response *resp,
                      int retransmit_ms)
{
    bool sent = false;
    int rc = pipeline_impl(
        [&](request *r) {
            if (sent)
            {
                return false;
            }
            sent = true;
            r->bytes = std::move(req);
            return true;
        },
        [&](const request &, const response &) {
            *resp_bytes = last_resp_;
            return 0;
        },
        1, retransmit_ms);
    if (rc)
    {
        return rc;
    }
    if (!parse(*resp_bytes, resp))
    {
        return EIO;
    }
    return resp->ret < 0 ? resp->err : 0;
}

int client::call(std::vector<uint8_t> req, std::vector<uint8_t> *resp_bytes, response *resp)
{
    return call_impl(std::move(req), resp_bytes, resp, cfg_.retransmit_ms);
}

int client::open(std::string_view path, int native_flags, uint32_t mode, int32_t *fd)
{
    auto b = begin(wire::op_open);
    writer w(b);
    w.u32(config::open_flags_to_wire(native_flags));
    w.u32(mode);
    w.str(path);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e && fd)
    {
        *fd = r.ret;
    }
    return e;
}

namespace
{
std::vector<uint8_t> with_handle(std::vector<uint8_t> b, int32_t h)
{
    writer(b).i32(h);
    return b;
}
}  // namespace

int client::close(int32_t fd)
{
    std::vector<uint8_t> rb;
    response r;
    return call(with_handle(begin(wire::op_close), fd), &rb, &r);
}

int client::closedir(int32_t dir)
{
    std::vector<uint8_t> rb;
    response r;
    return call(with_handle(begin(wire::op_closedir), dir), &rb, &r);
}

int client::fsync(int32_t fd)
{
    std::vector<uint8_t> rb;
    response r;
    return call(with_handle(begin(wire::op_fsync), fd), &rb, &r);
}

int client::ftruncate(int32_t fd, uint64_t len)
{
    auto b = with_handle(begin(wire::op_ftruncate), fd);
    writer(b).u64(len);
    std::vector<uint8_t> rb;
    response r;
    return call(std::move(b), &rb, &r);
}

int client::pread(int32_t fd, uint64_t off, uint32_t len, std::vector<uint8_t> *out)
{
    auto b = with_handle(begin(wire::op_read), fd);
    writer w(b);
    w.u64(off);
    w.u32(len);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e && out)
    {
        out->assign(r.payload, r.payload + std::min<size_t>(r.payload_n, static_cast<size_t>(r.ret)));
    }
    return e;
}

int client::pwrite(int32_t fd, uint64_t off, const uint8_t *data, size_t n, uint32_t *written)
{
    if (n > write_chunk())
    {
        return EMSGSIZE;
    }
    auto b = with_handle(begin(wire::op_write), fd);
    writer w(b);
    w.u64(off);
    w.u32(static_cast<uint32_t>(n));
    w.bytes(data, n);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e && written)
    {
        *written = static_cast<uint32_t>(r.ret);
    }
    return e;
}

int client::lseek(int32_t fd, int64_t off, uint8_t whence, int64_t *pos)
{
    auto b = with_handle(begin(wire::op_lseek), fd);
    writer w(b);
    w.i64(off);
    w.u8(whence);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e)
    {
        reader rd(r.payload, r.payload_n);
        const int64_t p = rd.i64();
        if (rd.failed())
        {
            return EIO;
        }
        if (pos)
        {
            *pos = p;
        }
    }
    return e;
}

int client::fstat(int32_t fd, wire::stat_block *st)
{
    std::vector<uint8_t> rb;
    response r;
    const int e = call(with_handle(begin(wire::op_fstat), fd), &rb, &r);
    if (!e)
    {
        reader rd(r.payload, r.payload_n);
        const auto s = rd.stat();
        if (rd.failed())
        {
            return EIO;
        }
        if (st)
        {
            *st = s;
        }
    }
    return e;
}

int client::stat(std::string_view path, wire::stat_block *st)
{
    auto b = begin(wire::op_stat);
    writer(b).str(path);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e)
    {
        reader rd(r.payload, r.payload_n);
        const auto s = rd.stat();
        if (rd.failed())
        {
            return EIO;
        }
        if (st)
        {
            *st = s;
        }
    }
    return e;
}

int client::unlink(std::string_view path)
{
    auto b = begin(wire::op_unlink);
    writer(b).str(path);
    std::vector<uint8_t> rb;
    response r;
    return call(std::move(b), &rb, &r);
}

int client::rmdir(std::string_view path)
{
    auto b = begin(wire::op_rmdir);
    writer(b).str(path);
    std::vector<uint8_t> rb;
    response r;
    return call(std::move(b), &rb, &r);
}

int client::mkdir(std::string_view path, uint32_t mode)
{
    auto b = begin(wire::op_mkdir);
    writer w(b);
    w.u32(mode);
    w.str(path);
    std::vector<uint8_t> rb;
    response r;
    return call(std::move(b), &rb, &r);
}

int client::opendir(std::string_view path, int32_t *dir)
{
    auto b = begin(wire::op_opendir);
    writer(b).str(path);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (!e && dir)
    {
        *dir = r.ret;
    }
    return e;
}

int client::readdir(int32_t dir, bool with_stat, std::vector<wire::dir_entry> *out, bool *end)
{
    auto b = with_handle(begin(wire::op_readdir), dir);
    writer(b).u8(with_stat ? wire::readdir_stat : 0);
    std::vector<uint8_t> rb;
    response r;
    const int e = call(std::move(b), &rb, &r);
    if (e)
    {
        return e;
    }
    reader rd(r.payload, r.payload_n);
    for (int32_t i = 0; i < r.ret; ++i)
    {
        wire::dir_entry d;
        d.type = rd.u8();
        d.size = rd.u64();
        d.mtime = rd.i64();
        const uint8_t len = rd.u8();
        d.name.assign(rd.bytes(len));
        if (rd.failed())
        {
            return EIO;
        }
        if (out)
        {
            out->push_back(std::move(d));
        }
    }
    if (end)
    {
        *end = r.ret == 0;
    }
    return 0;
}

int client::rename(std::string_view from, std::string_view to)
{
    if (from.size() > 0xffff)
    {
        return ENAMETOOLONG;
    }
    auto b = begin(wire::op_rename);
    writer w(b);
    w.u16(static_cast<uint16_t>(from.size()));
    w.str(from);
    w.str(to);
    std::vector<uint8_t> rb;
    response r;
    return call(std::move(b), &rb, &r);
}

int client::info(wire::info_block *out)
{
    std::vector<uint8_t> rb;
    response r;
    const int e = call(begin(wire::op_info), &rb, &r);
    if (e)
    {
        return e;
    }
    reader rd(r.payload, r.payload_n);
    wire::info_block i;
    i.max_datagram = rd.u32();
    i.max_path = rd.u32();
    i.max_handles = rd.u32();
    i.reply_cache = rd.u32();
    i.idle_timeout_ms = rd.u32();
    i.crc_max = rd.u32();
    i.open_handles = rd.u32();
    if (rd.failed())
    {
        return EIO;
    }
    if (out)
    {
        *out = i;
    }
    return 0;
}

int client::crc32(int32_t fd, uint64_t off, uint64_t len, uint32_t seed, uint32_t *crc, uint64_t *n)
{
    auto b = with_handle(begin(wire::op_crc32), fd);
    writer w(b);
    w.u64(off);
    w.u64(len);
    w.u32(seed);
    std::vector<uint8_t> rb;
    response r;
    const int e = call_impl(std::move(b), &rb, &r, std::max(cfg_.retransmit_ms, k_crc_retransmit_ms));
    if (e)
    {
        return e;
    }
    reader rd(r.payload, r.payload_n);
    const uint32_t c = rd.u32();
    const uint64_t got = rd.u64();
    if (rd.failed())
    {
        return EIO;
    }
    if (crc)
    {
        *crc = c;
    }
    if (n)
    {
        *n = got;
    }
    return 0;
}

int client::list(std::string_view path, bool with_stat, std::vector<wire::dir_entry> *out)
{
    int32_t dir = -1;
    if (const int e = opendir(path, &dir))
    {
        return e;
    }
    bool end = false;
    int e = 0;
    while (!end && !e)
    {
        e = readdir(dir, with_stat, out, &end);
    }
    const int ce = closedir(dir);
    return e ? e : ce;
}

int client::read_all(int32_t fd, uint64_t size, const std::function<bool(const uint8_t *, size_t)> &sink,
                     const progress_fn &progress)
{
    const uint32_t chunk = static_cast<uint32_t>(read_chunk());
    uint64_t next_off = 0;
    uint64_t delivered = 0;
    uint64_t end = size; // lowered when the file turns out shorter
    std::deque<std::pair<uint64_t, uint32_t>> redo;  // short reads' remainders
    std::map<uint64_t, std::vector<uint8_t>> ready;  // out-of-order data
    std::map<uint64_t, uint32_t> asked;              // off -> len of requests in flight
    bool stop = false;

    auto make = [&](request *r, uint64_t off, uint32_t len) {
        auto b = with_handle(begin(wire::op_read), fd);
        writer w(b);
        w.u64(off);
        w.u32(len);
        r->bytes = std::move(b);
        r->tag = off;
        asked[off] = len;
    };
    const int rc = pipeline(
        [&](request *r) {
            if (stop)
            {
                return false;
            }
            if (!redo.empty())
            {
                const auto [off, len] = redo.front();
                redo.pop_front();
                make(r, off, len);
                return true;
            }
            if (next_off >= end)
            {
                return false;
            }
            const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(chunk, end - next_off));
            make(r, next_off, len);
            next_off += len;
            return true;
        },
        [&](const request &q, const response &resp) -> int {
            if (resp.ret < 0)
            {
                return resp.err;
            }
            const uint64_t off = q.tag;
            const uint32_t len = asked[off];
            asked.erase(off);
            const auto got = static_cast<uint32_t>(std::min<size_t>(resp.payload_n, static_cast<size_t>(resp.ret)));
            if (got == 0)
            {
                end = std::min(end, off); // the file ended early
                return 0;
            }
            ready[off].assign(resp.payload, resp.payload + got);
            if (got < len)
            {
                redo.emplace_back(off + got, len - got);
            }
            while (!ready.empty() && ready.begin()->first == delivered)
            {
                auto &v = ready.begin()->second;
                if (!sink(v.data(), v.size()))
                {
                    stop = true;
                    return ECANCELED;
                }
                delivered += v.size();
                ready.erase(ready.begin());
                if (progress)
                {
                    progress(delivered, size);
                }
            }
            return 0;
        },
        cfg_.window);
    if (rc)
    {
        return rc;
    }
    return delivered == end ? (end == size ? 0 : EIO) : EIO;
}

int client::write_all(int32_t fd, uint64_t size,
                      const std::function<bool(uint64_t off, uint8_t *buf, size_t n)> &source,
                      const progress_fn &progress)
{
    const size_t chunk = write_chunk();
    uint64_t next_off = 0;
    uint64_t done_bytes = 0;
    std::deque<std::pair<uint64_t, uint32_t>> redo;
    std::map<uint64_t, uint32_t> asked;
    std::vector<uint8_t> buf(chunk);
    int src_err = 0;

    auto make = [&](request *r, uint64_t off, uint32_t len) -> bool {
        if (!source(off, buf.data(), len))
        {
            src_err = EIO;
            return false;
        }
        auto b = with_handle(begin(wire::op_write), fd);
        writer w(b);
        w.u64(off);
        w.u32(len);
        w.bytes(buf.data(), len);
        r->bytes = std::move(b);
        r->tag = off;
        asked[off] = len;
        return true;
    };
    const int rc = pipeline(
        [&](request *r) {
            if (src_err)
            {
                return false;
            }
            if (!redo.empty())
            {
                const auto [off, len] = redo.front();
                redo.pop_front();
                return make(r, off, len);
            }
            if (next_off >= size)
            {
                return false;
            }
            const auto len = static_cast<uint32_t>(std::min<uint64_t>(chunk, size - next_off));
            const uint64_t off = next_off;
            next_off += len;
            return make(r, off, len);
        },
        [&](const request &q, const response &resp) -> int {
            if (resp.ret < 0)
            {
                return resp.err;
            }
            const uint64_t off = q.tag;
            const uint32_t len = asked[off];
            asked.erase(off);
            const auto put = static_cast<uint32_t>(resp.ret);
            if (put == 0 && len > 0)
            {
                return ENOSPC;
            }
            if (put < len)
            {
                redo.emplace_back(off + put, len - put);
            }
            done_bytes += std::min(put, len);
            if (progress)
            {
                progress(done_bytes, size);
            }
            return 0;
        },
        cfg_.window);
    if (rc)
    {
        return rc;
    }
    return src_err ? src_err : (done_bytes == size ? 0 : EIO);
}

int client::crc_all(int32_t fd, uint64_t size, uint32_t *crc)
{
    uint32_t c = 0;
    uint64_t off = 0;
    while (off < size)
    {
        uint32_t piece = 0;
        uint64_t n = 0;
        if (const int e = crc32(fd, off, std::min<uint64_t>(k_crc_piece, size - off), c, &piece, &n))
        {
            return e;
        }
        if (n == 0)
        {
            return EIO; // shorter than size
        }
        c = piece;
        off += n;
    }
    if (crc)
    {
        *crc = c;
    }
    return 0;
}

}  // namespace udpfs
