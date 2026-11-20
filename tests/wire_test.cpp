#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include "udpfs/config.hpp"
#include "udpfs/wire.hpp"

using namespace udpfs::wire;

TEST(Crc32, KnownValueAndChaining)
{
    const char *s = "123456789";
    EXPECT_EQ(0xcbf43926u, crc32(0, s, 9));
    EXPECT_EQ(crc32(0, s, 9), crc32(crc32(0, s, 4), s + 4, 5));
    EXPECT_EQ(0u, crc32(0, s, 0));
}

TEST(Wire, WriterReaderRoundTripLittleEndian)
{
    std::vector<uint8_t> b;
    writer w(b);
    w.u8(0x12);
    w.u16(0x3456);
    w.u32(0x789abcde);
    w.i32(-2);
    w.u64(0x0102030405060708ull);
    w.i64(-3);
    w.str("hi");
    ASSERT_EQ(1u + 2 + 4 + 4 + 8 + 8 + 2, b.size());
    EXPECT_EQ(0x56, b[1]);
    EXPECT_EQ(0x34, b[2]);
    reader r(b.data(), b.size());
    EXPECT_EQ(0x12, r.u8());
    EXPECT_EQ(0x3456, r.u16());
    EXPECT_EQ(0x789abcdeu, r.u32());
    EXPECT_EQ(-2, r.i32());
    EXPECT_EQ(0x0102030405060708ull, r.u64());
    EXPECT_EQ(-3, r.i64());
    EXPECT_EQ("hi", r.bytes());
    EXPECT_FALSE(r.failed());
    EXPECT_EQ(0u, r.u32());
    EXPECT_TRUE(r.failed());
}

TEST(PosixMap, ErrnoAndFlagsByName)
{
    using namespace udpfs::config;
    EXPECT_EQ(e_noent, errno_to_wire(ENOENT));
    EXPECT_EQ(e_notsup, errno_to_wire(ENOTSUP));
    EXPECT_EQ(e_io, errno_to_wire(12345));
    EXPECT_EQ(ENOENT, errno_from_wire(e_noent));
    EXPECT_EQ(EIO, errno_from_wire(999));
    EXPECT_EQ(O_WRONLY | O_CREAT | O_TRUNC, open_flags_from_wire(o_wronly | o_creat | o_trunc));
    EXPECT_EQ(-1, open_flags_from_wire(0x400)); // O_APPEND is not on the wire
    EXPECT_EQ(o_rdwr | o_excl | o_creat, open_flags_to_wire(O_RDWR | O_EXCL | O_CREAT));
}
