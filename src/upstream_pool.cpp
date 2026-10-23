#include "upstream_pool.hpp"

#include <boost/asio/error.hpp>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace apigate::detail
{
namespace
{
void diagnostic(const char* event, int code) noexcept
{
    if (std::fprintf(stderr,
                     "{\"level\":\"error\",\"payload\":{\"event\":\"%s\",\"error_code\":%d}}\n",
                     event, code) < 0)
    {
        std::clearerr(stderr);
    }
}
}  // namespace

UpstreamConnection::UpstreamConnection(const boost::asio::any_io_executor& executor,
                                       UpstreamConfig backend,
                                       std::shared_ptr<UpstreamTlsContext> context)
    : backend_(std::move(backend)), context_(std::move(context)), plain_(executor)
{
    if (backend_.scheme == UpstreamScheme::https)
    {
        if (!context_)
        {
            throw std::invalid_argument("HTTPS connection requires validated trust");
        }
        tls_ = std::make_unique<TlsStream>(std::move(plain_), context_->context);
    }
}
UpstreamConnection::~UpstreamConnection()
{
    close();
}
boost::beast::tcp_stream& UpstreamConnection::transport() noexcept
{
    return tls_ ? boost::beast::get_lowest_layer(*tls_) : plain_;
}
UpstreamConnection::TlsStream* UpstreamConnection::tls() noexcept
{
    return tls_.get();
}
bool UpstreamConnection::matches(const UpstreamConfig& backend,
                                 const std::shared_ptr<UpstreamTlsContext>& context) const noexcept
{
    return backend_.host == backend.host && backend_.port == backend.port &&
           backend_.scheme == backend.scheme && context_.get() == context.get();
}
bool UpstreamConnection::clean() noexcept
{
    auto& socket = transport().socket();
    if (!socket.is_open() || (tls_ && SSL_has_pending(tls_->native_handle()) != 0))
    {
        return false;
    }
    boost::system::error_code error;
    const auto available = socket.available(error);
    return !error && available == 0;
}
void UpstreamConnection::close() noexcept
{
    auto& socket = transport().socket();
    if (!socket.is_open())
    {
        return;
    }
    boost::system::error_code error;
    const auto cancelled = socket.cancel(error);
    if (cancelled && cancelled != boost::asio::error::not_connected)
    {
        diagnostic("http_upstream_pool_cancel_failed", cancelled.value());
    }
    error.clear();
    const auto shutdown = socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    if (shutdown && shutdown != boost::asio::error::not_connected)
    {
        diagnostic("http_upstream_pool_shutdown_failed", shutdown.value());
    }
    error.clear();
    const auto closed = socket.close(error);
    if (closed)
    {
        diagnostic("http_upstream_pool_close_failed", closed.value());
    }
}

UpstreamPool::UpstreamPool(const boost::asio::any_io_executor& executor) : cleanup_(executor)
{
}
UpstreamPool::~UpstreamPool()
{
    stop();
}
UpstreamLease UpstreamPool::take(const UpstreamConfig& backend,
                                 const std::shared_ptr<UpstreamTlsContext>& context,
                                 Clock::time_point now)
{
    if (stopped_)
    {
        return {};
    }
    expire(now);
    for (auto& entry : idle_)
    {
        if (!entry)
        {
            continue;
        }
        auto& slot = *entry;
        if (slot.connection->matches(backend, context))
        {
            // Data may arrive after put(). Never hand an idle unsolicited response to
            // a new exchange. No request has been sent, so another candidate is safe.
            if (!slot.connection->clean())
            {
                entry.reset();
                continue;
            }
            auto connection = std::move(slot.connection);
            entry.reset();
            return connection;
        }
    }
    return {};
}
void UpstreamPool::put(UpstreamLease connection, Clock::time_point now) noexcept
{
    if (!connection || stopped_ || !connection->clean())
    {
        return;
    }
    try
    {
        expire(now);
        for (auto& entry : idle_)
        {
            if (!entry)
            {
                const auto deadline = now > Clock::time_point::max() - idle_timeout
                                          ? Clock::time_point::max()
                                          : now + idle_timeout;
                connection->transport().expires_never();
                entry.emplace(Idle{std::move(connection), deadline});
                arm_cleanup();
                return;
            }
        }
        // Full: the incoming lease is closed by destruction, without evicting busy work.
    }
    catch (...)
    {
        // A failed cleanup timer must never leave idle transports without active cleanup.
        diagnostic("http_upstream_pool_cleanup_failed", 0);
        stop();
    }
}
void UpstreamPool::expire(Clock::time_point now) noexcept
{
    for (auto& entry : idle_)
    {
        if (!entry)
        {
            continue;
        }
        const auto& slot = *entry;
        if (slot.deadline <= now)
        {
            entry.reset();
        }
    }
}
void UpstreamPool::arm_cleanup()
{
    if (stopped_ || timer_pending_)
    {
        return;
    }
    std::optional<Clock::time_point> next;
    for (const auto& entry : idle_)
    {
        if (!entry)
        {
            continue;
        }
        const auto& slot = *entry;
        if (!next || slot.deadline < *next)
        {
            next = slot.deadline;
        }
    }
    if (!next)
    {
        return;
    }
    const auto deadline = *next;
    cleanup_.expires_at(deadline);
    timer_pending_ = true;
    cleanup_.async_wait(
        [weak = weak_from_this()](const boost::system::error_code& error) noexcept
        {
            const auto self = weak.lock();
            if (!self || self->stopped_)
            {
                return;
            }
            self->timer_pending_ = false;
            if (error)
            {
                if (error != boost::asio::error::operation_aborted)
                {
                    diagnostic("http_upstream_pool_timer_failed", error.value());
                    self->stop();
                }
                return;
            }
            self->expire(Clock::now());
            try
            {
                self->arm_cleanup();
            }
            catch (...)
            {
                diagnostic("http_upstream_pool_cleanup_failed", 0);
                self->stop();
            }
        });
}
void UpstreamPool::stop() noexcept
{
    if (stopped_)
    {
        return;
    }
    stopped_ = true;
    try
    {
        static_cast<void>(cleanup_.cancel());
    }
    catch (...)
    {
        diagnostic("http_upstream_pool_timer_cancel_failed", 0);
    }
    for (auto& entry : idle_)
    {
        entry.reset();
    }
}
std::size_t UpstreamPool::idle_count() const noexcept
{
    std::size_t count = 0;
    for (const auto& entry : idle_)
    {
        if (entry)
        {
            ++count;
        }
    }
    return count;
}
}  // namespace apigate::detail
