// A connected UDP socket as a udpfs transport (POSIX sockets).
#pragma once

#include <string>

#include "udpfs/client.hpp"

namespace udpfs
{

class udp_transport final : public transport
{
public:
    udp_transport() = default;
    ~udp_transport() override;
    udp_transport(const udp_transport &) = delete;
    udp_transport &operator=(const udp_transport &) = delete;

    // host (name or IPv4) and port; 0 or a native errno.
    int connect(const std::string &host, uint16_t port);

    bool send(const uint8_t *data, size_t n) override;
    int recv(uint8_t *buf, size_t cap, int timeout_ms) override;

private:
    int fd_ = -1;
};

}  // namespace udpfs
