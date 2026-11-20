#include "udpfs_idf/service.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <bfc/default_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace udpfs_idf
{
namespace
{

const char *TAG = "udpfs";

using reactor_t = bfc::default_reactor<>;
using queue_t = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using queue_registry = bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

// At most this many requests per source per reactor pass.
constexpr size_t k_max_batch = 8;
constexpr int64_t k_poll_ms = 1000;

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

int bind_udp(const std::string &host_port)
{
    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos)
    {
        return -1;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::atoi(host_port.c_str() + colon + 1)));
    if (addr.sin_port == 0 || inet_pton(AF_INET, host_port.substr(0, colon).c_str(), &addr.sin_addr) != 1)
    {
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

struct service::impl
{
    config cfg;
    std::unique_ptr<udpfs::server> srv;
    std::shared_ptr<reactor_t> reactor;
    std::shared_ptr<queue_t> to_l3, from_l3;
    int fd = -1;
    std::vector<uint8_t> rx;
    std::vector<uint8_t> resp;
    bool started = false;
    // Stats copied after each request, for readers on other threads.
    std::atomic<uint64_t> requests {0}, cached {0}, dup_dropped {0}, malformed {0}, errors {0}, idle_closes {0}, bytes_read {0},
        bytes_written {0};
    std::atomic<uint32_t> open_handles {0};

    void publish()
    {
        const auto &s = srv->stats();
        requests = s.requests;
        cached = s.cached;
        dup_dropped = s.dup_dropped;
        malformed = s.malformed;
        errors = s.errors;
        idle_closes = s.idle_closes;
        bytes_read = s.bytes_read;
        bytes_written = s.bytes_written;
        open_handles = static_cast<uint32_t>(srv->open_handles());
    }

    void tick()
    {
        srv->poll(now_ms());
        publish();
        reactor->get_timer().wait_ms(k_poll_ms, [this] { tick(); });
    }
};

service::service() : impl_(std::make_unique<impl>()) {}
service::~service() = default;

bool service::start(const config &cfg)
{
    impl &m = *impl_;
    if (m.started)
    {
        return false;
    }
    m.cfg = cfg;
    if (cfg.to_l3.empty() != cfg.from_l3.empty())
    {
        ESP_LOGE(TAG, "the upstream needs both queue names");
        return false;
    }
    if (!cfg.to_l3.empty())
    {
        m.to_l3 = queue_registry::get_instance().get(cfg.to_l3);
        m.from_l3 = queue_registry::get_instance().get(cfg.from_l3);
        if (!m.to_l3 || !m.from_l3)
        {
            ESP_LOGE(TAG, "no queues %s / %s", cfg.to_l3.c_str(), cfg.from_l3.c_str());
            return false;
        }
    }
    if (!cfg.udp.empty())
    {
        m.fd = bind_udp(cfg.udp);
        if (m.fd < 0)
        {
            ESP_LOGE(TAG, "cannot bind %s", cfg.udp.c_str());
            return false;
        }
    }
    udpfs::server_config sc = cfg.server;
    if (sc.handle_seed == 0)
    {
        sc.handle_seed = esp_random() | 1u;
    }
    m.srv = std::make_unique<udpfs::server>(sc);
    m.rx.resize(sc.max_datagram + 64);
    m.reactor = std::make_shared<reactor_t>();
    if (!cfg.reactor.empty() && !bfc::default_reactor_registry::get_instance().add(cfg.reactor, m.reactor))
    {
        ESP_LOGW(TAG, "reactor name %s taken; running unregistered", cfg.reactor.c_str());
    }

    if (m.from_l3)
    {
        m.reactor->add_read_rdy(*m.from_l3, [&m] {
            for (size_t i = 0; i < k_max_batch; ++i)
            {
                auto packet = m.from_l3->pop();
                if (!packet)
                {
                    break;
                }
                if (m.srv->handle(reinterpret_cast<const uint8_t *>(packet->data()), packet->size(), now_ms(),
                                  &m.resp))
                {
                    m.to_l3->push(bfc::shared_sized_buffer::copy_from(m.resp.data(), m.resp.size()));
                }
            }
            m.publish();
        });
        m.from_l3->set_reactor(m.reactor);
    }
    if (m.fd >= 0)
    {
        m.reactor->add_read_rdy(m.fd, [&m] {
            for (size_t i = 0; i < k_max_batch; ++i)
            {
                sockaddr_in from {};
                socklen_t len = sizeof(from);
                const ssize_t n = recvfrom(m.fd, m.rx.data(), m.rx.size(), MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr *>(&from), &len);
                if (n <= 0)
                {
                    break;
                }
                if (m.srv->handle(m.rx.data(), static_cast<size_t>(n), now_ms(), &m.resp))
                {
                    sendto(m.fd, m.resp.data(), m.resp.size(), 0, reinterpret_cast<const sockaddr *>(&from), len);
                }
            }
            m.publish();
        });
    }
    m.reactor->get_timer().wait_ms(k_poll_ms, [&m] { m.tick(); });

    esp_pthread_cfg_t tc = esp_pthread_get_default_config();
    esp_pthread_cfg_t prev {};
    const bool had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;
    tc.thread_name = "udpfs";
    tc.stack_size = cfg.stack_bytes;
    tc.prio = cfg.prio;
    tc.pin_to_core = cfg.core;
    tc.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&tc);
    std::thread([r = m.reactor] { r->run(); }).detach();
    const esp_pthread_cfg_t restore = had_prev ? prev : esp_pthread_get_default_config();
    esp_pthread_set_cfg(&restore);

    m.started = true;
    printf("udpfs: %s%s%s (max_datagram %u, max_handles %u)\n", m.from_l3 ? "upstream" : "",
           m.from_l3 && m.fd >= 0 ? " and " : "", m.fd >= 0 ? cfg.udp.c_str() : "",
           static_cast<unsigned>(sc.max_datagram), static_cast<unsigned>(sc.max_handles));
    return true;
}

unsigned service::open_handles() const
{
    return impl_->open_handles.load();
}

std::string service::stats_line() const
{
    const impl &m = *impl_;
    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "requests=%" PRIu64 " cached=%" PRIu64 " dup_dropped=%" PRIu64 " malformed=%" PRIu64 " errors=%" PRIu64
                  " idle_closes=%" PRIu64 " bytes_read=%" PRIu64 " bytes_written=%" PRIu64 " open=%u",
                  m.requests.load(), m.cached.load(), m.dup_dropped.load(), m.malformed.load(), m.errors.load(), m.idle_closes.load(),
                  m.bytes_read.load(), m.bytes_written.load(), static_cast<unsigned>(m.open_handles.load()));
    return buf;
}

}  // namespace udpfs_idf
