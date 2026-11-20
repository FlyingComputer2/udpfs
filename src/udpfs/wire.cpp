#include "udpfs/wire.hpp"

#include <array>

namespace udpfs::wire
{
namespace
{
constexpr std::array<uint32_t, 256> make_table()
{
    std::array<uint32_t, 256> t {};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
        {
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}
constexpr std::array<uint32_t, 256> k_table = make_table();
}  // namespace

uint32_t crc32(uint32_t seed, const void *data, size_t n)
{
    const auto *p = static_cast<const uint8_t *>(data);
    uint32_t c = ~seed;
    for (size_t i = 0; i < n; ++i)
    {
        c = k_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace udpfs::wire
