// The project's udpfs configuration.
//
// The top-level project (the firmware or application, never a library that
// udpfs is built into) provides <udpfs_config.hpp> and puts its directory on
// the include path (UDPFS_CONFIG_DIR in CMake). udpfs has no fallback: a
// missing config is a compile error, so no part of a build maps errno or open
// flags differently from the rest.
//
// <udpfs_config.hpp> defines:
//
//   UDPFS_CONFIG_ID   a string naming the config (for diagnostics)
//
//   in namespace udpfs::config, the maps between wire values (Linux numbering,
//   docs/protocol.md) and this C library's:
//     uint32_t errno_to_wire(int native);        // unknown -> wire EIO
//     int      errno_from_wire(uint32_t wire);   // unknown -> native EIO
//     int      open_flags_from_wire(uint32_t);   // -1 for unknown bits
//     uint32_t open_flags_to_wire(int native);
//
// <udpfs/posix_map.hpp> implements all four by macro name, which fits any
// POSIX C library (glibc, ESP-IDF's newlib); a config normally forwards to it:
//
//   #pragma once
//   #include <udpfs/posix_map.hpp>
//   #define UDPFS_CONFIG_ID "my-firmware"
//   namespace udpfs::config
//   {
//   using namespace udpfs::posix_map;
//   }
#pragma once

#if defined(__has_include)
#if !__has_include(<udpfs_config.hpp>)
#error "udpfs needs a project <udpfs_config.hpp> (see udpfs/config.hpp); set UDPFS_CONFIG_DIR"
#endif
#endif

#include <udpfs_config.hpp>

#include <cstdint>
#include <type_traits>

#ifndef UDPFS_CONFIG_ID
#error "udpfs_config.hpp must define UDPFS_CONFIG_ID"
#endif

namespace udpfs::config
{
static_assert(std::is_invocable_r_v<uint32_t, decltype(errno_to_wire), int>,
              "udpfs::config::errno_to_wire(int) -> uint32_t");
static_assert(std::is_invocable_r_v<int, decltype(errno_from_wire), uint32_t>,
              "udpfs::config::errno_from_wire(uint32_t) -> int");
static_assert(std::is_invocable_r_v<int, decltype(open_flags_from_wire), uint32_t>,
              "udpfs::config::open_flags_from_wire(uint32_t) -> int");
static_assert(std::is_invocable_r_v<uint32_t, decltype(open_flags_to_wire), int>,
              "udpfs::config::open_flags_to_wire(int) -> uint32_t");
}  // namespace udpfs::config
