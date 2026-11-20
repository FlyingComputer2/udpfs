// udpfs client: the protocol's operations over a datagram transport, with
// retransmission (docs/protocol.md) and a window of requests in flight for bulk
// reads, writes and checksums. Errors are native errno values (mapped from the
// wire through the project's <udpfs_config.hpp>); ETIMEDOUT when the server
// did not answer within give_up_ms. One thread per client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "udpfs/wire.hpp"

namespace udpfs
{

// One datagram per call.
class transport
{
public:
    virtual ~transport() = default;
    // False when the datagram could not be sent (the client treats it as lost).
    virtual bool send(const uint8_t *data, size_t n) = 0;
    // > 0: one datagram of that size; 0: nothing within timeout_ms; < 0: error.
    virtual int recv(uint8_t *buf, size_t cap, int timeout_ms) = 0;
};

struct client_config
{
    size_t max_datagram = 1400;
    // Resend timer (RFC 6298): smoothed round trip + 4 x its variation, from
    // responses to requests sent once; initial_rto_ms until the first sample,
    // clamped to [min_rto_ms, max_rto_ms]. Each resend of a request doubles its
    // own timer (up to max_rto_ms).
    // On the rover link (video sharing the air) the round trip varies 30-200 ms:
    // below 200 ms the timer fired on late responses, and the resends cost more
    // than they saved (photo 6-8 s instead of 3-4 s).
    int initial_rto_ms = 400;
    int min_rto_ms = 200;
    int max_rto_ms = 3000;
    // ETIMEDOUT when a request has no response this long after its first send.
    int give_up_ms = 15000;
    // Bulk requests in flight.
    size_t window = 16;
    // Requests up to this size (READ, STAT, ...: tens of bytes) are sent twice,
    // dup_delay_ms apart, against request loss; a server drops the copy it has
    // just answered (its dup_window_ms). 0 = never (default: on the rover link
    // losses come in bursts, both copies went together, and it was slower).
    size_t dup_max_bytes = 0;
    int dup_delay_ms = 3;
    // First request id; 0 = random.
    uint32_t reqid_seed = 0;
};

struct client_stats
{
    uint64_t requests = 0;
    uint64_t sends = 0; // including retransmissions
    uint64_t retransmits = 0;
    uint64_t dups = 0;  // second copies of small requests
    uint64_t stray = 0; // responses matching no request in flight (also the copy's)
    double srtt_ms = 0; // smoothed round trip, 0 before the first sample
    int rto_ms = 0;     // the resend timer now
};

// Called with bytes done and total (0 when unknown) during bulk transfers.
using progress_fn = std::function<void(uint64_t done, uint64_t total)>;

class client
{
public:
    explicit client(transport &t, client_config cfg = {});

    int open(std::string_view path, int native_flags, uint32_t mode, int32_t *fd);
    int close(int32_t fd);
    // Up to len bytes at off (fewer near the end of file, 0 = end).
    int pread(int32_t fd, uint64_t off, uint32_t len, std::vector<uint8_t> *out);
    int pwrite(int32_t fd, uint64_t off, const uint8_t *data, size_t n, uint32_t *written);
    int lseek(int32_t fd, int64_t off, uint8_t whence, int64_t *pos);
    int fstat(int32_t fd, wire::stat_block *st);
    int stat(std::string_view path, wire::stat_block *st);
    int unlink(std::string_view path);
    int mkdir(std::string_view path, uint32_t mode);
    int rmdir(std::string_view path);
    int opendir(std::string_view path, int32_t *dir);
    // Appends the next entries; *end is set when the directory is done.
    int readdir(int32_t dir, bool with_stat, std::vector<wire::dir_entry> *out, bool *end);
    int closedir(int32_t dir);
    int rename(std::string_view from, std::string_view to);
    int fsync(int32_t fd);
    int ftruncate(int32_t fd, uint64_t len);
    int info(wire::info_block *out);
    // Debug: the server's open handles.
    int list_open(std::vector<wire::open_entry> *out);
    // One CRC32 request (at most the server's crc_max bytes).
    int crc32(int32_t fd, uint64_t off, uint64_t len, uint32_t seed, uint32_t *crc, uint64_t *n);

    // Whole-directory listing (OPENDIR / READDIR / CLOSEDIR).
    int list(std::string_view path, bool with_stat, std::vector<wire::dir_entry> *out);
    // Reads the file from 0 to its end with the window in flight; sink gets the
    // bytes in order and may return false to stop (ECANCELED).
    int read_all(int32_t fd, uint64_t size, const std::function<bool(const uint8_t *, size_t)> &sink,
                 const progress_fn &progress = {});
    // Writes size bytes from source(off, buf, n), which fills exactly n bytes.
    int write_all(int32_t fd, uint64_t size,
                  const std::function<bool(uint64_t off, uint8_t *buf, size_t n)> &source,
                  const progress_fn &progress = {});
    // CRC32 of [0, size) with the window in flight.
    int crc_all(int32_t fd, uint64_t size, uint32_t *crc);

    [[nodiscard]] const client_stats &stats() const { return stats_; }
    [[nodiscard]] const client_config &config() const { return cfg_; }
    // Bytes one READ returns / one WRITE carries at most.
    [[nodiscard]] size_t read_chunk() const { return cfg_.max_datagram - wire::k_response_header; }
    [[nodiscard]] size_t write_chunk() const
    {
        return cfg_.max_datagram - wire::k_request_header - wire::k_write_fixed;
    }

private:
    struct request
    {
        std::vector<uint8_t> bytes;
        uint64_t tag = 0;
    };
    struct response
    {
        int32_t ret = -1;
        int err = 0; // native errno
        const uint8_t *payload = nullptr;
        size_t payload_n = 0;
    };
    // Builds a request datagram with a new reqid.
    std::vector<uint8_t> begin(uint8_t op);
    // Sends one request and waits for its response (retransmitting); the
    // response's payload points into *resp_bytes.
    int call(std::vector<uint8_t> req, std::vector<uint8_t> *resp_bytes, response *resp);
    int call_impl(std::vector<uint8_t> req, std::vector<uint8_t> *resp_bytes, response *resp,
                  int rto_floor_ms);
    // Runs requests from next() with up to window in flight; done() gets each
    // response (any order; its payload is valid during the call) and may return
    // an errno to stop.
    int pipeline(const std::function<bool(request *)> &next,
                 const std::function<int(const request &, const response &)> &done, size_t window);
    // rto_floor_ms: the resend timer is at least this (CRC32 computes a while).
    int pipeline_impl(const std::function<bool(request *)> &next,
                      const std::function<int(const request &, const response &)> &done,
                      size_t window, int rto_floor_ms);
    [[nodiscard]] int rto(int floor_ms) const;
    void rtt_sample(double ms);
    static bool parse(const std::vector<uint8_t> &bytes, response *r);

    transport &t_;
    client_config cfg_;
    uint32_t next_reqid_;
    client_stats stats_;
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> last_resp_; // the response done() is looking at
    bool have_rtt_ = false;
    double srtt_ = 0;
    double rttvar_ = 0;
};

}  // namespace udpfs
