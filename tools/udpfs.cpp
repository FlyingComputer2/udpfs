// udpfs: an scp-like command line client.
//
//   udpfs [options] cp [-r] SRC DST       one of SRC / DST is remote
//   udpfs [options] get REMOTE [LOCAL]    cp REMOTE LOCAL (default .)
//   udpfs [options] put LOCAL REMOTE      cp LOCAL REMOTE
//   udpfs [options] ls [-l] REMOTE
//   udpfs [options] stat | cat | crc | rm | mkdir | rmdir REMOTE...
//   udpfs [options] mv REMOTE NEWPATH     NEWPATH on the same server
//   udpfs [options] info HOST[:PORT]
//   udpfs [options] list_open HOST[:PORT]    debug: the server's open handles
//
// REMOTE is HOST[:PORT]:/absolute/path (PORT 2440 by default), as with scp an
// argument with ':' before its first '/'. Transfers are checked with CRC32 on
// the server (--no-verify skips it); a download goes to <file>.part first.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "udpfs/client.hpp"
#include "udpfs/udp_transport.hpp"

namespace
{

using udpfs::wire::dir_entry;
using udpfs::wire::stat_block;

constexpr uint16_t k_default_port = 2440;

struct options
{
    udpfs::client_config cc;
    bool quiet = false;
    bool verify = true;
};

struct remote
{
    std::string host;
    uint16_t port = k_default_port;
    std::string path;
};

bool is_remote(const std::string &a)
{
    const auto colon = a.find(':');
    const auto slash = a.find('/');
    return colon != std::string::npos && colon > 0 && (slash == std::string::npos || colon < slash);
}

bool parse_remote(const std::string &a, remote *r)
{
    if (!is_remote(a))
    {
        return false;
    }
    const auto c1 = a.find(':');
    r->host = a.substr(0, c1);
    std::string rest = a.substr(c1 + 1);
    if (!rest.empty() && rest[0] != '/')
    {
        const auto c2 = rest.find(':');
        if (c2 == std::string::npos)
        {
            return false;
        }
        const long port = std::strtol(rest.substr(0, c2).c_str(), nullptr, 10);
        if (port <= 0 || port > 65535)
        {
            return false;
        }
        r->port = static_cast<uint16_t>(port);
        rest = rest.substr(c2 + 1);
    }
    r->path = rest.empty() ? "/" : rest;
    return r->path[0] == '/';
}

// HOST or HOST:PORT (for info).
bool parse_host(const std::string &a, remote *r)
{
    const auto c = a.find(':');
    r->host = a.substr(0, c);
    if (c != std::string::npos)
    {
        const long port = std::strtol(a.c_str() + c + 1, nullptr, 10);
        if (port <= 0 || port > 65535)
        {
            return false;
        }
        r->port = static_cast<uint16_t>(port);
    }
    return !r->host.empty();
}

std::string base_name(const std::string &p)
{
    std::string s = p;
    while (s.size() > 1 && s.back() == '/')
    {
        s.pop_back();
    }
    const auto slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string join(const std::string &dir, const std::string &name)
{
    if (dir.empty() || dir.back() == '/')
    {
        return dir + name;
    }
    return dir + "/" + name;
}

int fail(const std::string &what, int e)
{
    std::fprintf(stderr, "udpfs: %s: %s\n", what.c_str(), std::strerror(e));
    return e;
}

int64_t mono_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class progress
{
public:
    progress(const options &o, std::string name) : quiet_(o.quiet), name_(std::move(name)), t0_(mono_ms()) {}
    void operator()(uint64_t done, uint64_t total)
    {
        const int64_t now = mono_ms();
        if (quiet_ || (now - last_ < 250 && done != total))
        {
            return;
        }
        last_ = now;
        const double s = std::max<int64_t>(now - t0_, 1) / 1000.0;
        std::fprintf(stderr, "\r%s %" PRIu64 "/%" PRIu64 " B %3d%% %.1f KB/s   ", name_.c_str(), done, total,
                     total ? static_cast<int>(done * 100 / total) : 100, done / 1024.0 / s);
    }
    void finish(uint64_t total)
    {
        (*this)(total, total);
        if (!quiet_)
        {
            std::fprintf(stderr, "\n");
        }
    }

private:
    bool quiet_;
    std::string name_;
    int64_t t0_;
    int64_t last_ = 0;
};

uint32_t local_crc(int fd, uint64_t size, int *err)
{
    std::vector<uint8_t> buf(64 * 1024);
    uint32_t c = 0;
    uint64_t off = 0;
    while (off < size)
    {
        const ssize_t n = ::pread(fd, buf.data(), std::min<uint64_t>(buf.size(), size - off), static_cast<off_t>(off));
        if (n <= 0)
        {
            *err = n < 0 ? errno : EIO;
            return 0;
        }
        c = udpfs::wire::crc32(c, buf.data(), static_cast<size_t>(n));
        off += static_cast<uint64_t>(n);
    }
    *err = 0;
    return c;
}

class session
{
public:
    explicit session(const options &o) : o_(o) {}

    int connect(const remote &r)
    {
        if (cl_ && r.host == host_ && r.port == port_)
        {
            return 0;
        }
        if (const int e = t_.connect(r.host, r.port))
        {
            return fail(r.host + ":" + std::to_string(r.port), e);
        }
        host_ = r.host;
        port_ = r.port;
        cl_ = std::make_unique<udpfs::client>(t_, o_.cc);
        return 0;
    }
    udpfs::client &c() { return *cl_; }

    int download_file(const std::string &rpath, uint64_t size, const std::string &lpath)
    {
        int32_t fd = -1;
        if (const int e = c().open(rpath, O_RDONLY, 0, &fd))
        {
            return fail(rpath, e);
        }
        const std::string part = lpath + ".part";
        const int lfd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd < 0)
        {
            const int e = errno;
            c().close(fd);
            return fail(part, e);
        }
        uint32_t crc = 0;
        int werr = 0;
        progress p(o_, base_name(rpath));
        int e = c().read_all(
            fd, size,
            [&](const uint8_t *d, size_t n) {
                crc = udpfs::wire::crc32(crc, d, n);
                if (::write(lfd, d, n) != static_cast<ssize_t>(n))
                {
                    werr = errno ? errno : EIO;
                    return false;
                }
                return true;
            },
            [&](uint64_t done, uint64_t total) { p(done, total); });
        if (!e)
        {
            p.finish(size);
        }
        if (e == ECANCELED && werr)
        {
            e = werr;
        }
        if (!e && o_.verify)
        {
            uint32_t rcrc = 0;
            e = c().crc_all(fd, size, &rcrc);
            if (!e && rcrc != crc)
            {
                std::fprintf(stderr, "udpfs: %s: CRC mismatch (local %08x, remote %08x)\n", rpath.c_str(), crc, rcrc);
                e = EIO;
            }
        }
        c().close(fd);
        if (::close(lfd) != 0 && !e)
        {
            e = errno;
        }
        if (e)
        {
            ::unlink(part.c_str());
            return fail(rpath, e);
        }
        if (::rename(part.c_str(), lpath.c_str()) != 0)
        {
            return fail(lpath, errno);
        }
        return 0;
    }

    int upload_file(const std::string &lpath, const std::string &rpath)
    {
        const int lfd = ::open(lpath.c_str(), O_RDONLY);
        if (lfd < 0)
        {
            return fail(lpath, errno);
        }
        struct stat st {};
        ::fstat(lfd, &st);
        const auto size = static_cast<uint64_t>(st.st_size);
        int32_t fd = -1;
        if (const int e = c().open(rpath, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777, &fd))
        {
            ::close(lfd);
            return fail(rpath, e);
        }
        progress p(o_, base_name(lpath));
        int e = c().write_all(
            fd, size,
            [&](uint64_t off, uint8_t *buf, size_t n) {
                return ::pread(lfd, buf, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n);
            },
            [&](uint64_t done, uint64_t total) { p(done, total); });
        if (!e)
        {
            p.finish(size);
            e = c().fsync(fd);
        }
        const int ce = c().close(fd);
        e = e ? e : ce;
        if (!e && o_.verify)
        {
            int le = 0;
            const uint32_t crc = local_crc(lfd, size, &le);
            int32_t rfd = -1;
            e = le ? le : c().open(rpath, O_RDONLY, 0, &rfd);
            if (!e)
            {
                uint32_t rcrc = 0;
                e = c().crc_all(rfd, size, &rcrc);
                c().close(rfd);
                if (!e && rcrc != crc)
                {
                    std::fprintf(stderr, "udpfs: %s: CRC mismatch (local %08x, remote %08x)\n", rpath.c_str(), crc,
                                 rcrc);
                    e = EIO;
                }
            }
        }
        ::close(lfd);
        return e ? fail(rpath, e) : 0;
    }

    int download(const std::string &rpath, const std::string &ldst, bool recursive)
    {
        stat_block st;
        if (const int e = c().stat(rpath, &st))
        {
            return fail(rpath, e);
        }
        struct stat lst {};
        const bool ldir = ::stat(ldst.c_str(), &lst) == 0 && S_ISDIR(lst.st_mode);
        const std::string target = ldir ? join(ldst, base_name(rpath)) : ldst;
        if (st.type != udpfs::wire::type_dir)
        {
            return download_file(rpath, st.size, target);
        }
        if (!recursive)
        {
            return fail(rpath, EISDIR);
        }
        if (::mkdir(target.c_str(), 0755) != 0 && errno != EEXIST)
        {
            return fail(target, errno);
        }
        std::vector<dir_entry> entries;
        if (const int e = c().list(rpath, false, &entries))
        {
            return fail(rpath, e);
        }
        int rc = 0;
        for (const auto &d : entries)
        {
            const std::string child = join(rpath, d.name);
            const int e = download(child, join(target, d.name), true);
            rc = rc ? rc : e;
        }
        return rc;
    }

    int upload(const std::string &lsrc, const std::string &rdst, bool recursive)
    {
        struct stat lst {};
        if (::stat(lsrc.c_str(), &lst) != 0)
        {
            return fail(lsrc, errno);
        }
        stat_block rst;
        const bool rdir = c().stat(rdst, &rst) == 0 && rst.type == udpfs::wire::type_dir;
        const std::string target = rdir ? join(rdst, base_name(lsrc)) : rdst;
        if (!S_ISDIR(lst.st_mode))
        {
            return upload_file(lsrc, target);
        }
        if (!recursive)
        {
            return fail(lsrc, EISDIR);
        }
        if (const int e = c().mkdir(target, 0755); e && e != EEXIST)
        {
            return fail(target, e);
        }
        DIR *d = ::opendir(lsrc.c_str());
        if (!d)
        {
            return fail(lsrc, errno);
        }
        int rc = 0;
        while (const dirent *de = ::readdir(d))
        {
            const std::string name = de->d_name;
            if (name == "." || name == "..")
            {
                continue;
            }
            const int e = upload(join(lsrc, name), join(target, name), true);
            rc = rc ? rc : e;
        }
        ::closedir(d);
        return rc;
    }

private:
    const options &o_;
    udpfs::udp_transport t_;
    std::unique_ptr<udpfs::client> cl_;
    std::string host_;
    uint16_t port_ = 0;
};

const char *type_char(uint8_t t)
{
    return t == udpfs::wire::type_dir ? "d" : t == udpfs::wire::type_file ? "-" : "?";
}

std::string time_str(int64_t t)
{
    char buf[32];
    const time_t tt = static_cast<time_t>(t);
    std::tm tm {};
    localtime_r(&tt, &tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

int usage()
{
    std::fprintf(stderr,
                 "usage: udpfs [-w window] [-t retransmit_ms] [-n tries] [-q] [--no-verify] COMMAND ...\n"
                 "  cp [-r] SRC DST        one side remote: HOST[:PORT]:/path (port %u)\n"
                 "  get REMOTE [LOCAL]     put LOCAL REMOTE\n"
                 "  ls [-l] REMOTE         stat | cat | crc | rm | mkdir | rmdir REMOTE...\n"
                 "  mv REMOTE NEWPATH      info HOST[:PORT]      list_open HOST[:PORT]\n",
                 k_default_port);
    return 2;
}

}  // namespace

int main(int argc, char **argv)
{
    options o;
    std::vector<std::string> args(argv + 1, argv + argc);
    size_t i = 0;
    for (; i < args.size() && args[i].size() > 1 && args[i][0] == '-'; ++i)
    {
        const std::string &a = args[i];
        if ((a == "-w" || a == "-t" || a == "-n") && i + 1 < args.size())
        {
            const long v = std::strtol(args[++i].c_str(), nullptr, 10);
            if (v <= 0)
            {
                return usage();
            }
            if (a == "-w")
            {
                o.cc.window = static_cast<size_t>(v);
            }
            else if (a == "-t")
            {
                o.cc.retransmit_ms = static_cast<int>(v);
            }
            else
            {
                o.cc.tries = static_cast<int>(v);
            }
        }
        else if (a == "-q")
        {
            o.quiet = true;
        }
        else if (a == "--no-verify")
        {
            o.verify = false;
        }
        else
        {
            return usage();
        }
    }
    if (i >= args.size())
    {
        return usage();
    }
    std::string cmd = args[i++];
    std::vector<std::string> rest(args.begin() + static_cast<long>(i), args.end());
    session s(o);

    bool recursive = false;
    bool longfmt = false;
    if (!rest.empty() && (rest[0] == "-r" || rest[0] == "-l"))
    {
        recursive = rest[0] == "-r";
        longfmt = rest[0] == "-l";
        rest.erase(rest.begin());
    }

    if (cmd == "get" && (rest.size() == 1 || rest.size() == 2))
    {
        cmd = "cp";
        if (rest.size() == 1)
        {
            rest.push_back(".");
        }
    }
    else if (cmd == "put" && rest.size() == 2)
    {
        cmd = "cp";
    }

    if (cmd == "cp" && rest.size() == 2)
    {
        remote r;
        if (parse_remote(rest[0], &r) && !is_remote(rest[1]))
        {
            return s.connect(r) ? 1 : (s.download(r.path, rest[1], recursive) ? 1 : 0);
        }
        if (parse_remote(rest[1], &r) && !is_remote(rest[0]))
        {
            return s.connect(r) ? 1 : (s.upload(rest[0], r.path, recursive) ? 1 : 0);
        }
        std::fprintf(stderr, "udpfs: cp: exactly one of SRC and DST must be HOST[:PORT]:/path\n");
        return 2;
    }
    if (cmd == "info" && rest.size() == 1)
    {
        remote r;
        if (!parse_host(rest[0], &r) || s.connect(r))
        {
            return 1;
        }
        udpfs::wire::info_block in;
        if (const int e = s.c().info(&in))
        {
            return fail(rest[0], e) ? 1 : 0;
        }
        std::printf("max_datagram=%u max_path=%u max_handles=%u reply_cache=%u idle_timeout_ms=%u crc_max=%u "
                    "open_handles=%u\n",
                    in.max_datagram, in.max_path, in.max_handles, in.reply_cache, in.idle_timeout_ms, in.crc_max,
                    in.open_handles);
        return 0;
    }
    if (cmd == "list_open" && rest.size() == 1)
    {
        remote r;
        if (!parse_host(rest[0], &r) || s.connect(r))
        {
            return 1;
        }
        std::vector<udpfs::wire::open_entry> open;
        if (const int e = s.c().list_open(&open))
        {
            fail(rest[0], e);
            return 1;
        }
        for (const auto &o : open)
        {
            std::printf("%11d %s pos=%" PRId64 " %s\n", o.handle, o.type == udpfs::wire::type_dir ? "dir " : "file",
                        o.pos, o.path.c_str());
        }
        return 0;
    }
    if (cmd == "mv" && rest.size() == 2)
    {
        remote r;
        if (!parse_remote(rest[0], &r) || rest[1].empty() || rest[1][0] != '/' || s.connect(r))
        {
            return usage();
        }
        const int e = s.c().rename(r.path, rest[1]);
        return e ? (fail(r.path, e), 1) : 0;
    }

    if (rest.empty())
    {
        return usage();
    }
    int rc = 0;
    for (const auto &a : rest)
    {
        remote r;
        if (!parse_remote(a, &r))
        {
            std::fprintf(stderr, "udpfs: %s: not HOST[:PORT]:/path\n", a.c_str());
            return 2;
        }
        if (s.connect(r))
        {
            return 1;
        }
        auto &c = s.c();
        int e = 0;
        if (cmd == "ls")
        {
            stat_block st;
            e = c.stat(r.path, &st);
            if (!e && st.type != udpfs::wire::type_dir)
            {
                std::printf("%s %12" PRIu64 " %s %s\n", type_char(st.type), st.size, time_str(st.mtime).c_str(),
                            r.path.c_str());
                continue;
            }
            std::vector<dir_entry> entries;
            e = e ? e : c.list(r.path, longfmt, &entries);
            for (const auto &d : entries)
            {
                if (longfmt)
                {
                    std::printf("%s %12" PRIu64 " %s %s\n", type_char(d.type), d.size, time_str(d.mtime).c_str(),
                                d.name.c_str());
                }
                else
                {
                    std::printf("%s%s\n", d.name.c_str(), d.type == udpfs::wire::type_dir ? "/" : "");
                }
            }
        }
        else if (cmd == "stat")
        {
            stat_block st;
            e = c.stat(r.path, &st);
            if (!e)
            {
                std::printf("%s type=%s mode=%03o size=%" PRIu64 " mtime=%s\n", r.path.c_str(),
                            st.type == udpfs::wire::type_dir ? "dir" : st.type == udpfs::wire::type_file ? "file" : "other",
                            st.mode, st.size, time_str(st.mtime).c_str());
            }
        }
        else if (cmd == "cat" || cmd == "crc")
        {
            stat_block st;
            int32_t fd = -1;
            e = c.stat(r.path, &st);
            e = e ? e : c.open(r.path, O_RDONLY, 0, &fd);
            if (!e && cmd == "cat")
            {
                e = c.read_all(fd, st.size, [](const uint8_t *d, size_t n) {
                    return std::fwrite(d, 1, n, stdout) == n;
                });
            }
            else if (!e)
            {
                uint32_t crc = 0;
                e = c.crc_all(fd, st.size, &crc);
                if (!e)
                {
                    std::printf("%08x %" PRIu64 " %s\n", crc, st.size, r.path.c_str());
                }
            }
            if (fd >= 0)
            {
                c.close(fd);
            }
        }
        else if (cmd == "rm")
        {
            e = c.unlink(r.path);
        }
        else if (cmd == "mkdir")
        {
            e = c.mkdir(r.path, 0755);
        }
        else if (cmd == "rmdir")
        {
            e = c.rmdir(r.path);
        }
        else
        {
            return usage();
        }
        if (e)
        {
            fail(r.path, e);
            rc = 1;
        }
    }
    return rc;
}
