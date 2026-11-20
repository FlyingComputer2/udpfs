// udpfs server over POSIX calls (open / read / lseek / opendir / ...): one
// request datagram in, at most one response datagram out. No I/O of its own
// beyond the filesystem and no threads; the caller feeds it datagrams and
// calls poll() now and then for the idle timeout. Not thread-safe: one caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "udpfs/wire.hpp"

namespace udpfs
{

struct server_config
{
    size_t max_datagram = 1400;
    size_t max_path = 255;
    size_t max_handles = 4;
    size_t reply_cache = 32;
    uint32_t idle_timeout_ms = 30000;
    uint32_t crc_max = 1024 * 1024;
    // Start of the handle numbers, in [1, 2^30); 0 = derived from the clock. A
    // firmware passes a hardware random number.
    uint32_t handle_seed = 0;
};

struct server_stats
{
    uint64_t requests = 0;
    uint64_t cached = 0;      // answered from the reply cache
    uint64_t malformed = 0;   // EPROTO / EMSGSIZE / too short
    uint64_t errors = 0;      // ret -1 for any other reason
    uint64_t idle_closes = 0; // idle timeouts that closed something
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
};

class server
{
public:
    explicit server(server_config cfg = {});
    ~server();
    server(const server &) = delete;
    server &operator=(const server &) = delete;

    // Handles one request at monotonic time now_ms. Returns the response, or
    // false when the datagram gets none (shorter than a request header).
    bool handle(const uint8_t *req, size_t n, uint64_t now_ms, std::vector<uint8_t> *resp);
    // Closes every handle and clears the reply cache once idle_timeout_ms has
    // passed since the last request.
    void poll(uint64_t now_ms);
    // Closes every handle and clears the reply cache now.
    void reset();

    [[nodiscard]] const server_stats &stats() const { return stats_; }
    [[nodiscard]] size_t open_handles() const { return handles_.size(); }
    [[nodiscard]] const server_config &config() const { return cfg_; }

private:
    struct handle_t
    {
        bool dir = false;
        int fd = -1;
        void *dirp = nullptr; // DIR*
        int64_t pos = 0;      // LSEEK's position
        std::string path;     // as opened (LIST_OPEN; READDIR's stat for directories)
        bool have_pending = false;
        wire::dir_entry pending; // read from the DIR but not yet sent
    };
    struct cached_reply
    {
        uint8_t op;
        int32_t handle;
        uint32_t reqid;
        std::vector<uint8_t> resp;
    };

    void dispatch(uint8_t op, uint32_t reqid, wire::reader &r, std::vector<uint8_t> *resp);
    int32_t new_handle(handle_t h);
    handle_t *find(int32_t h);
    void close_handle(int32_t h);
    int path_arg(std::string_view p, std::string *out) const;

    server_config cfg_;
    std::map<int32_t, handle_t> handles_;
    int32_t next_handle_ = 1;
    std::deque<cached_reply> cache_;
    uint64_t last_request_ms_ = 0;
    bool any_request_ = false;
    std::vector<uint8_t> buf_; // READ / CRC32 scratch
    server_stats stats_;
};

}  // namespace udpfs
