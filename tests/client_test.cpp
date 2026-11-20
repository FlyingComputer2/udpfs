#include <gtest/gtest.h>

#include <fcntl.h>

#include <deque>
#include <fstream>
#include <random>
#include <thread>

#include "test_util.hpp"
#include "udpfs/client.hpp"
#include "udpfs/server.hpp"

using namespace udpfs;

namespace
{

// The server behind a link that drops, duplicates and reorders datagrams.
class lossy_link final : public transport
{
public:
    lossy_link(server &s, double loss, double dup, uint32_t seed) : srv_(s), loss_(loss), dup_(dup), rng_(seed) {}

    bool send(const uint8_t *data, size_t n) override
    {
        ++sent;
        if (chance(loss_))
        {
            return true;
        }
        std::vector<uint8_t> resp;
        const auto now = ++clock_ms_;
        const int copies = chance(dup_) ? 2 : 1;
        for (int i = 0; i < copies; ++i)
        {
            if (srv_.handle(data, n, now, &resp) && !chance(loss_))
            {
                if (!q_.empty() && chance(0.3))
                {
                    q_.insert(q_.begin(), resp); // reordered
                }
                else
                {
                    q_.push_back(resp);
                }
            }
        }
        return true;
    }

    int recv(uint8_t *buf, size_t cap, int timeout_ms) override
    {
        if (q_.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
            return 0;
        }
        auto r = std::move(q_.front());
        q_.pop_front();
        const size_t n = std::min(cap, r.size());
        std::copy(r.begin(), r.begin() + static_cast<long>(n), buf);
        return static_cast<int>(n);
    }

    uint64_t sent = 0;

private:
    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng_) < p; }
    server &srv_;
    double loss_;
    double dup_;
    std::mt19937 rng_;
    std::deque<std::vector<uint8_t>> q_;
    uint64_t clock_ms_ = 0;
};

client_config fast()
{
    client_config c;
    c.retransmit_ms = 3;
    c.tries = 200;
    c.window = 8;
    return c;
}

std::vector<uint8_t> pattern(size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
    {
        v[i] = static_cast<uint8_t>((i * 131) ^ (i >> 7));
    }
    return v;
}

}  // namespace

TEST(ClientTest, BulkWriteReadAndCrcOverALossyLink)
{
    udpfs_test::temp_dir dir;
    server srv;
    lossy_link link(srv, 0.2, 0.1, 1);
    client c(link, fast());
    const auto data = pattern(300 * 1024 + 77);
    const std::string p = dir / "bulk.bin";

    int32_t fd = -1;
    ASSERT_EQ(0, c.open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644, &fd));
    ASSERT_EQ(0, c.write_all(fd, data.size(), [&](uint64_t off, uint8_t *buf, size_t n) {
        std::copy(data.begin() + static_cast<long>(off), data.begin() + static_cast<long>(off + n), buf);
        return true;
    }));
    ASSERT_EQ(0, c.fsync(fd));
    ASSERT_EQ(0, c.close(fd));

    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(data, disk);

    ASSERT_EQ(0, c.open(p, O_RDONLY, 0, &fd));
    int64_t size = 0;
    ASSERT_EQ(0, c.lseek(fd, 0, wire::seek_end, &size));
    EXPECT_EQ(static_cast<int64_t>(data.size()), size);
    std::vector<uint8_t> back;
    ASSERT_EQ(0, c.read_all(fd, static_cast<uint64_t>(size), [&](const uint8_t *d, size_t n) {
        back.insert(back.end(), d, d + n);
        return true;
    }));
    EXPECT_EQ(data, back);
    uint32_t crc = 0;
    ASSERT_EQ(0, c.crc_all(fd, data.size(), &crc));
    EXPECT_EQ(wire::crc32(0, data.data(), data.size()), crc);
    EXPECT_EQ(0, c.close(fd));
    EXPECT_GT(c.stats().retransmits, 0u);
    EXPECT_EQ(0u, srv.open_handles());
}

TEST(ClientTest, ListAndPathOpsOverALossyLink)
{
    udpfs_test::temp_dir dir;
    server srv;
    lossy_link link(srv, 0.25, 0.2, 2);
    client c(link, fast());
    ASSERT_EQ(0, c.mkdir(dir / "d", 0755));
    for (int i = 0; i < 150; ++i)
    {
        int32_t fd = -1;
        ASSERT_EQ(0, c.open(dir / ("d/f" + std::to_string(i)), O_WRONLY | O_CREAT | O_EXCL, 0644, &fd));
        ASSERT_EQ(0, c.close(fd));
    }
    std::vector<wire::dir_entry> entries;
    ASSERT_EQ(0, c.list(dir / "d", true, &entries));
    EXPECT_EQ(150u, entries.size());
    ASSERT_EQ(0, c.rename(dir / "d/f0", dir / "d/g0"));
    wire::stat_block st;
    EXPECT_EQ(ENOENT, c.stat(dir / "d/f0", &st));
    EXPECT_EQ(0, c.stat(dir / "d/g0", &st));
    EXPECT_EQ(ENOTEMPTY, c.rmdir(dir / "d"));
    for (const auto &e : entries)
    {
        const std::string name = e.name == "f0" ? "g0" : e.name;
        ASSERT_EQ(0, c.unlink(dir / ("d/" + name)));
    }
    EXPECT_EQ(0, c.rmdir(dir / "d"));
    wire::info_block in;
    ASSERT_EQ(0, c.info(&in));
    EXPECT_EQ(0u, in.open_handles);
}

TEST(ClientTest, NoServerTimesOut)
{
    struct silent final : transport
    {
        bool send(const uint8_t *, size_t) override { return true; }
        int recv(uint8_t *, size_t, int) override { return 0; }
    } s;
    client_config cfg;
    cfg.retransmit_ms = 1;
    cfg.tries = 3;
    client c(s, cfg);
    wire::stat_block st;
    EXPECT_EQ(ETIMEDOUT, c.stat("/x", &st));
}
