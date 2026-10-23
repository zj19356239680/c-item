#ifndef APIGATE_UPSTREAM_POOL_HPP_
#define APIGATE_UPSTREAM_POOL_HPP_

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <memory>
#include <optional>

#include "apigate/config.hpp"
#include "upstream_tls.hpp"

namespace apigate::detail
{

// An exclusive lease is a unique_ptr. Destruction discards the transport; only put() caches it.
// Contains no HTTP message, body permit, resolver, or exchange deadline.
class UpstreamConnection
{
   public:
    using TlsStream = boost::beast::ssl_stream<boost::beast::tcp_stream>;
    UpstreamConnection(const boost::asio::any_io_executor& executor, UpstreamConfig backend,
                       std::shared_ptr<UpstreamTlsContext> context);
    ~UpstreamConnection();
    UpstreamConnection(const UpstreamConnection&) = delete;
    UpstreamConnection& operator=(const UpstreamConnection&) = delete;

    [[nodiscard]] boost::beast::tcp_stream& transport() noexcept;
    [[nodiscard]] TlsStream* tls() noexcept;
    [[nodiscard]] bool matches(const UpstreamConfig& backend,
                               const std::shared_ptr<UpstreamTlsContext>& context) const noexcept;
    [[nodiscard]] bool clean() noexcept;
    void close() noexcept;

   private:
    UpstreamConfig backend_;
    std::shared_ptr<UpstreamTlsContext> context_;
    boost::beast::tcp_stream plain_;
    std::unique_ptr<TlsStream> tls_;
};

using UpstreamLease = std::unique_ptr<UpstreamConnection>;

// Server-owned; all calls and callbacks are serialized on the single io_context.
class UpstreamPool : public std::enable_shared_from_this<UpstreamPool>
{
   public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t max_idle = 32;
    static constexpr auto idle_timeout = std::chrono::seconds{30};
    explicit UpstreamPool(const boost::asio::any_io_executor& executor);
    ~UpstreamPool();
    UpstreamPool(const UpstreamPool&) = delete;
    UpstreamPool& operator=(const UpstreamPool&) = delete;

    [[nodiscard]] UpstreamLease take(const UpstreamConfig& backend,
                                     const std::shared_ptr<UpstreamTlsContext>& context,
                                     Clock::time_point now = Clock::now());
    void put(UpstreamLease connection, Clock::time_point now = Clock::now()) noexcept;
    void expire(Clock::time_point now) noexcept;
    void stop() noexcept;
    [[nodiscard]] std::size_t idle_count() const noexcept;

   private:
    struct Idle
    {
        UpstreamLease connection;
        Clock::time_point deadline;
    };
    void arm_cleanup();
    std::array<std::optional<Idle>, max_idle> idle_;
    boost::asio::steady_timer cleanup_;
    bool timer_pending_{false};
    bool stopped_{false};
};

}  // namespace apigate::detail
#endif  // APIGATE_UPSTREAM_POOL_HPP_
