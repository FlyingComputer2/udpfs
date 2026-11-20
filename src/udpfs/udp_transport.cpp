#include "udpfs/udp_transport.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace udpfs
{

udp_transport::~udp_transport()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
}

int udp_transport::connect(const std::string &host, uint16_t port)
{
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res)
    {
        return EHOSTUNREACH;
    }
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        const int e = errno;
        freeaddrinfo(res);
        return e;
    }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        const int e = errno;
        ::close(fd);
        freeaddrinfo(res);
        return e;
    }
    freeaddrinfo(res);
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
    fd_ = fd;
    return 0;
}

bool udp_transport::send(const uint8_t *data, size_t n)
{
    return fd_ >= 0 && ::send(fd_, data, n, 0) == static_cast<ssize_t>(n);
}

int udp_transport::recv(uint8_t *buf, size_t cap, int timeout_ms)
{
    if (fd_ < 0)
    {
        return -1;
    }
    pollfd p {fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, timeout_ms);
    if (r <= 0)
    {
        return r == 0 || errno == EINTR ? 0 : -1;
    }
    const ssize_t n = ::recv(fd_, buf, cap, 0);
    if (n < 0)
    {
        // ECONNREFUSED: nothing listens yet; the request is resent.
        return errno == ECONNREFUSED || errno == EINTR ? 0 : -1;
    }
    return static_cast<int>(n);
}

}  // namespace udpfs
