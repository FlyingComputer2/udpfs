// Wire <-> native errno and open flags, paired by macro name. Each wire value
// (Linux numbering, docs/protocol.md) is matched with the native macro of the
// same name, so this compiles to the right table on glibc, ESP-IDF's newlib or
// any other POSIX C library.
#pragma once

#include <cerrno>
#include <cstdint>
#include <fcntl.h>

#include "udpfs/wire.hpp"

namespace udpfs::posix_map
{

struct errno_pair
{
    uint32_t wire;
    int native;
};

inline constexpr errno_pair k_errnos[] = {
    {wire::e_perm, EPERM},         {wire::e_noent, ENOENT},     {wire::e_io, EIO},
    {wire::e_badf, EBADF},         {wire::e_again, EAGAIN},     {wire::e_nomem, ENOMEM},
    {wire::e_acces, EACCES},       {wire::e_busy, EBUSY},       {wire::e_exist, EEXIST},
    {wire::e_xdev, EXDEV},         {wire::e_notdir, ENOTDIR},   {wire::e_isdir, EISDIR},
    {wire::e_inval, EINVAL},       {wire::e_nfile, ENFILE},     {wire::e_mfile, EMFILE},
    {wire::e_fbig, EFBIG},         {wire::e_nospc, ENOSPC},     {wire::e_rofs, EROFS},
    {wire::e_nametoolong, ENAMETOOLONG}, {wire::e_nosys, ENOSYS}, {wire::e_notempty, ENOTEMPTY},
    {wire::e_proto, EPROTO},       {wire::e_msgsize, EMSGSIZE}, {wire::e_notsup, ENOTSUP},
    {wire::e_timedout, ETIMEDOUT},
};

inline uint32_t errno_to_wire(int native)
{
    if (0 == native)
    {
        return 0;
    }
    for (const auto &p : k_errnos)
    {
        if (p.native == native)
        {
            return p.wire;
        }
    }
#if defined(EOPNOTSUPP)
    if (EOPNOTSUPP == native)
    {
        return wire::e_notsup;
    }
#endif
#if defined(EWOULDBLOCK)
    if (EWOULDBLOCK == native)
    {
        return wire::e_again;
    }
#endif
    return wire::e_io;
}

inline int errno_from_wire(uint32_t w)
{
    if (0 == w)
    {
        return 0;
    }
    for (const auto &p : k_errnos)
    {
        if (p.wire == w)
        {
            return p.native;
        }
    }
    return EIO;
}

inline int open_flags_from_wire(uint32_t w)
{
    if (0 != (w & ~wire::o_known))
    {
        return -1;
    }
    int f = 0;
    switch (w & wire::o_accmode)
    {
    case wire::o_rdonly:
        f = O_RDONLY;
        break;
    case wire::o_wronly:
        f = O_WRONLY;
        break;
    case wire::o_rdwr:
        f = O_RDWR;
        break;
    default:
        return -1;
    }
    if (w & wire::o_creat)
    {
        f |= O_CREAT;
    }
    if (w & wire::o_excl)
    {
        f |= O_EXCL;
    }
    if (w & wire::o_trunc)
    {
        f |= O_TRUNC;
    }
    return f;
}

inline uint32_t open_flags_to_wire(int native)
{
    uint32_t w = 0;
    switch (native & O_ACCMODE)
    {
    case O_WRONLY:
        w = wire::o_wronly;
        break;
    case O_RDWR:
        w = wire::o_rdwr;
        break;
    default:
        w = wire::o_rdonly;
        break;
    }
    if (native & O_CREAT)
    {
        w |= wire::o_creat;
    }
    if (native & O_EXCL)
    {
        w |= wire::o_excl;
    }
    if (native & O_TRUNC)
    {
        w |= wire::o_trunc;
    }
    return w;
}

}  // namespace udpfs::posix_map
