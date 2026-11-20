#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <set>

#include "test_util.hpp"
#include "udpfs/server.hpp"

using namespace udpfs;
using namespace udpfs::wire;

namespace
{

struct reply
{
    uint8_t op = 0;
    uint32_t reqid = 0;
    int32_t ret = 0;
    uint32_t err = 0;
    std::vector<uint8_t> payload;
};

class ServerTest : public ::testing::Test
{
protected:
    server srv {cfg()};
    udpfs_test::temp_dir dir;
    uint32_t reqid = 100;
    uint64_t now = 1000;

    static server_config cfg()
    {
        server_config c;
        c.max_handles = 4;
        c.reply_cache = 8;
        c.idle_timeout_ms = 5000;
        c.handle_seed = 7;
        return c;
    }

    std::vector<uint8_t> req(uint8_t op, uint32_t id)
    {
        std::vector<uint8_t> b;
        writer w(b);
        w.u8(op);
        w.u32(id);
        return b;
    }

    reply send(const std::vector<uint8_t> &b)
    {
        std::vector<uint8_t> out;
        EXPECT_TRUE(srv.handle(b.data(), b.size(), now, &out));
        reply r;
        reader rd(out.data(), out.size());
        r.op = rd.u8();
        r.reqid = rd.u32();
        r.ret = rd.i32();
        r.err = rd.u32();
        EXPECT_FALSE(rd.failed());
        const auto p = rd.bytes();
        r.payload.assign(p.begin(), p.end());
        return r;
    }

    reply open(const std::string &path, uint32_t flags, uint32_t id = 0)
    {
        auto b = req(op_open, id ? id : ++reqid);
        writer w(b);
        w.u32(flags);
        w.u32(0644);
        w.str(path);
        return send(b);
    }

    reply with_fd(uint8_t op, int32_t fd, uint32_t id = 0)
    {
        auto b = req(op, id ? id : ++reqid);
        writer(b).i32(fd);
        return send(b);
    }

    reply write(int32_t fd, uint64_t off, const std::string &data)
    {
        auto b = req(op_write, ++reqid);
        writer w(b);
        w.i32(fd);
        w.u64(off);
        w.u32(static_cast<uint32_t>(data.size()));
        w.str(data);
        return send(b);
    }

    reply read(int32_t fd, uint64_t off, uint32_t len)
    {
        auto b = req(op_read, ++reqid);
        writer w(b);
        w.i32(fd);
        w.u64(off);
        w.u32(len);
        return send(b);
    }

    reply path_op(uint8_t op, const std::string &path, uint32_t id = 0)
    {
        auto b = req(op, id ? id : ++reqid);
        if (op == op_mkdir)
        {
            writer(b).u32(0755);
        }
        writer(b).str(path);
        return send(b);
    }

    static std::string text(const reply &r) { return std::string(r.payload.begin(), r.payload.end()); }
};

}  // namespace

TEST_F(ServerTest, WriteReadSeekStat)
{
    const std::string p = dir / "a.txt";
    const reply o = open(p, o_rdwr | o_creat | o_trunc);
    ASSERT_GE(o.ret, 1);
    EXPECT_EQ(op_open | k_response_bit, o.op);
    EXPECT_EQ(5, write(o.ret, 0, "hello").ret);
    EXPECT_EQ(6, write(o.ret, 5, " world").ret);
    EXPECT_EQ("lo wo", text(read(o.ret, 3, 5)));
    EXPECT_EQ(0, read(o.ret, 11, 10).ret); // end of file

    auto b = req(op_lseek, ++reqid);
    writer w(b);
    w.i32(o.ret);
    w.i64(0);
    w.u8(seek_end);
    const reply s = send(b);
    ASSERT_EQ(0, s.ret);
    reader rd(s.payload.data(), s.payload.size());
    EXPECT_EQ(11, rd.i64());

    const reply st = path_op(op_stat, p);
    ASSERT_EQ(0, st.ret);
    reader sr(st.payload.data(), st.payload.size());
    const stat_block blk = sr.stat();
    EXPECT_EQ(type_file, blk.type);
    EXPECT_EQ(11u, blk.size);
    EXPECT_EQ(0, with_fd(op_close, o.ret).ret);
    EXPECT_EQ(e_badf, with_fd(op_close, o.ret).err);
}

TEST_F(ServerTest, ReadIsCappedByTheDatagram)
{
    const std::string p = dir / "big";
    {
        std::ofstream f(p, std::ios::binary);
        f << std::string(5000, 'x');
    }
    const reply o = open(p, o_rdonly);
    const reply r = read(o.ret, 0, 5000);
    EXPECT_EQ(static_cast<int32_t>(1400 - k_response_header), r.ret);
    EXPECT_EQ(static_cast<size_t>(r.ret), r.payload.size());
}

TEST_F(ServerTest, RetransmittedOpenAndUnlinkComeFromTheCache)
{
    const std::string p = dir / "c";
    const reply a = open(p, o_wronly | o_creat, 555);
    const reply b = open(p, o_wronly | o_creat, 555);
    EXPECT_EQ(a.ret, b.ret);
    EXPECT_EQ(1u, srv.open_handles());
    EXPECT_EQ(1u, srv.stats().cached);

    EXPECT_EQ(0, path_op(op_unlink, p, 777).ret);
    const reply again = path_op(op_unlink, p, 777);
    EXPECT_EQ(0, again.ret); // the cached success, not ENOENT
    EXPECT_EQ(e_noent, path_op(op_unlink, p).err);
}

TEST_F(ServerTest, HandlesAreNotReusedAndCapped)
{
    const std::string p = dir / "h";
    std::set<int32_t> seen;
    for (int i = 0; i < 20; ++i)
    {
        const reply o = open(p, o_wronly | o_creat);
        ASSERT_GE(o.ret, 1);
        EXPECT_TRUE(seen.insert(o.ret).second);
        with_fd(op_close, o.ret);
    }
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_GE(open(p, o_rdonly).ret, 1);
    }
    EXPECT_EQ(e_mfile, open(p, o_rdonly).err);
}

TEST_F(ServerTest, ReaddirPacksAndContinues)
{
    for (int i = 0; i < 200; ++i)
    {
        std::ofstream(dir / ("file_with_a_longish_name_" + std::to_string(i)));
    }
    ::mkdir((dir / "sub").c_str(), 0755);
    const reply d = path_op(op_opendir, dir.path());
    ASSERT_GE(d.ret, 1);
    std::set<std::string> names;
    int responses = 0;
    for (;;)
    {
        auto b = req(op_readdir, ++reqid);
        writer w(b);
        w.i32(d.ret);
        w.u8(readdir_stat);
        const reply r = send(b);
        ASSERT_GE(r.ret, 0);
        if (r.ret == 0)
        {
            break;
        }
        ++responses;
        EXPECT_LE(r.payload.size() + k_response_header, 1400u);
        reader rd(r.payload.data(), r.payload.size());
        for (int i = 0; i < r.ret; ++i)
        {
            const uint8_t type = rd.u8();
            rd.u64();
            rd.i64();
            const std::string name(rd.bytes(rd.u8()));
            EXPECT_EQ(name == "sub" ? type_dir : type_file, type) << name;
            EXPECT_TRUE(names.insert(name).second);
        }
        EXPECT_FALSE(rd.failed());
    }
    EXPECT_EQ(201u, names.size());
    EXPECT_GT(responses, 1);
    EXPECT_EQ(0, with_fd(op_closedir, d.ret).ret);
}

TEST_F(ServerTest, RetransmittedReaddirDoesNotSkip)
{
    for (int i = 0; i < 100; ++i)
    {
        std::ofstream(dir / ("n" + std::to_string(i)));
    }
    const reply d = path_op(op_opendir, dir.path());
    auto b = req(op_readdir, 4242);
    writer w(b);
    w.i32(d.ret);
    w.u8(0);
    const reply first = send(b);
    const reply again = send(b);
    EXPECT_EQ(first.ret, again.ret);
    EXPECT_EQ(first.payload, again.payload);
}

TEST_F(ServerTest, RenameMkdirRmdirTruncateCrcInfo)
{
    const std::string a = dir / "a";
    const std::string b = dir / "b";
    const reply o = open(a, o_rdwr | o_creat);
    write(o.ret, 0, "123456789");

    auto c = req(op_crc32, ++reqid);
    writer cw(c);
    cw.i32(o.ret);
    cw.u64(0);
    cw.u64(100);
    cw.u32(0);
    const reply cr = send(c);
    ASSERT_EQ(0, cr.ret);
    reader crd(cr.payload.data(), cr.payload.size());
    EXPECT_EQ(0xcbf43926u, crd.u32());
    EXPECT_EQ(9u, crd.u64());

    auto t = req(op_ftruncate, ++reqid);
    writer tw(t);
    tw.i32(o.ret);
    tw.u64(4);
    EXPECT_EQ(0, send(t).ret);
    EXPECT_EQ(0, with_fd(op_fsync, o.ret).ret);
    with_fd(op_close, o.ret);

    auto rn = req(op_rename, ++reqid);
    writer rw(rn);
    rw.u16(static_cast<uint16_t>(a.size()));
    rw.str(a);
    rw.str(b);
    EXPECT_EQ(0, send(rn).ret);
    EXPECT_EQ(e_noent, path_op(op_stat, a).err);
    struct stat st {};
    ASSERT_EQ(0, ::stat(b.c_str(), &st));
    EXPECT_EQ(4, st.st_size);

    EXPECT_EQ(0, path_op(op_mkdir, dir / "d").ret);
    EXPECT_EQ(e_exist, path_op(op_mkdir, dir / "d").err);
    EXPECT_EQ(0, path_op(op_rmdir, dir / "d").ret);

    const reply in = send(req(op_info, ++reqid));
    ASSERT_EQ(0, in.ret);
    reader ir(in.payload.data(), in.payload.size());
    EXPECT_EQ(1400u, ir.u32());
}

TEST_F(ServerTest, ProtocolErrors)
{
    EXPECT_EQ(e_inval, open("relative/path", o_rdonly).err);
    EXPECT_EQ(e_inval, open(dir / "x", 0x400).err); // O_APPEND
    EXPECT_EQ(e_nosys, send(req(0x7f, 1)).err);
    auto shortw = req(op_write, ++reqid);
    writer(shortw).i32(1);
    EXPECT_EQ(e_proto, send(shortw).err);
    std::vector<uint8_t> huge = req(op_stat, ++reqid);
    huge.resize(2000, 'a');
    EXPECT_EQ(e_msgsize, send(huge).err);
    std::vector<uint8_t> out;
    const uint8_t tiny[3] = {1, 2, 3};
    EXPECT_FALSE(srv.handle(tiny, sizeof(tiny), now, &out));
    EXPECT_EQ(e_isdir, open(dir.path(), o_rdonly).err);
}

TEST_F(ServerTest, ListOpenShowsHandles)
{
    const reply f = open(dir / "lo", o_rdwr | o_creat);
    auto b = req(op_lseek, ++reqid);
    writer w(b);
    w.i32(f.ret);
    w.i64(42);
    w.u8(seek_set);
    send(b);
    const reply d = path_op(op_opendir, dir.path());
    const reply l = send(req(op_list_open, ++reqid));
    ASSERT_EQ(2, l.ret);
    reader rd(l.payload.data(), l.payload.size());
    std::map<int32_t, std::pair<uint8_t, std::string>> seen;
    for (int i = 0; i < 2; ++i)
    {
        const int32_t h = rd.i32();
        const uint8_t t = rd.u8();
        const int64_t pos = rd.i64();
        const std::string path(rd.bytes(rd.u16()));
        if (h == f.ret)
        {
            EXPECT_EQ(42, pos);
        }
        seen[h] = {t, path};
    }
    EXPECT_FALSE(rd.failed());
    EXPECT_EQ(type_file, seen[f.ret].first);
    EXPECT_EQ(dir / "lo", seen[f.ret].second);
    EXPECT_EQ(type_dir, seen[d.ret].first);
    EXPECT_EQ(dir.path(), seen[d.ret].second);
}

TEST_F(ServerTest, IdleTimeoutClosesHandles)
{
    const reply o = open(dir / "i", o_wronly | o_creat);
    srv.poll(now + 4999);
    EXPECT_EQ(1u, srv.open_handles());
    srv.poll(now + 5000);
    EXPECT_EQ(0u, srv.open_handles());
    EXPECT_EQ(e_badf, with_fd(op_close, o.ret).err);
}
