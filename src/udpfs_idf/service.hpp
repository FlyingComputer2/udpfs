// udpfs on ESP-IDF: the server on a bfc reactor and thread of its own, fed by
// an L3 QUEUE_FORWARDING upstream (two named queues in bfc's queue registry)
// and/or a local UDP socket. Blocking file I/O stalls only this thread; the
// upstream's queue holds requests meanwhile and the client resends.
//
// The thread's stack is in internal RAM: any SPIFFS / SPI flash access (e.g.
// /winject) turns the cache off, and ESP-IDF then requires the calling task's
// stack to be in internal RAM. Start the service early at boot, while internal
// RAM is not yet fragmented (a late request can land in slow LP SRAM).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "udpfs/server.hpp"

namespace udpfs_idf
{

struct config
{
    // The upstream: L3's rx = to_l3 (responses), tx = from_l3 (requests).
    // Empty = none.
    std::string to_l3;
    std::string from_l3;
    // Local UDP endpoint, e.g. "0.0.0.0:2440", replying to each request's
    // sender. Empty = none.
    std::string udp;
    udpfs::server_config server;
    // The reactor's name in bfc::default_reactor_registry; empty = unregistered.
    std::string reactor = "udpfs";
    // Thread: internal-RAM stack (see above).
    size_t stack_bytes = 8 * 1024;
    int prio = 5;
    int core = -1; // -1 = any
};

class service
{
public:
    service();
    ~service();
    service(const service &) = delete;
    service &operator=(const service &) = delete;

    // Finds the queues, binds the socket and starts the thread; false on any
    // failure (logged). Once only; the service lives for the program.
    bool start(const config &cfg);
    // "requests=... cached=... malformed=... errors=... idle_closes=...
    // bytes_read=... bytes_written=... open=..." (approximate while busy).
    [[nodiscard]] std::string stats_line() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace udpfs_idf
