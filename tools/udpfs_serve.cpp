// udpfs_serve [HOST:]PORT: serves this machine's filesystem over UDP, replying
// to each request's sender. For tests and for a Linux board; no access
// control (the client acts as this process's user).
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "udpfs/server.hpp"

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: udpfs_serve [HOST:]PORT\n");
        return 2;
    }
    std::string a = argv[1];
    std::string host = "0.0.0.0";
    const auto c = a.rfind(':');
    if (c != std::string::npos)
    {
        host = a.substr(0, c);
        a = a.substr(c + 1);
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::atoi(a.c_str())));
    if (addr.sin_port == 0 || inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "udpfs_serve: bad address %s\n", argv[1]);
        return 2;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        std::perror("udpfs_serve: bind");
        return 1;
    }
    udpfs::server srv;
    std::vector<uint8_t> buf(65536);
    std::vector<uint8_t> resp;
    auto now_ms = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    };
    std::fprintf(stderr, "udpfs_serve: %s:%u\n", host.c_str(), ntohs(addr.sin_port));
    for (;;)
    {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 1000) > 0)
        {
            sockaddr_in from {};
            socklen_t len = sizeof(from);
            const ssize_t n = recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n > 0 && srv.handle(buf.data(), static_cast<size_t>(n), now_ms(), &resp))
            {
                sendto(fd, resp.data(), resp.size(), 0, reinterpret_cast<const sockaddr *>(&from), len);
            }
        }
        srv.poll(now_ms());
    }
}
