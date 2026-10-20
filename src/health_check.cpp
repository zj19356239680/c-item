#include "health_check.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "apigate/logging.hpp"
#include "http_proxy.hpp"
#include "upstream_tls.hpp"

namespace apigate::detail
{
namespace
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Clock = ProbeSchedule::Clock;

void diagnostic(const char* event) noexcept
{
    if (std::fprintf(stderr, "{\"level\":\"error\",\"payload\":{\"event\":\"%s\"}}\n", event) < 0)
    {
        std::clearerr(stderr);
    }
}

struct ProbeTarget
{
    std::size_t route;
    std::size_t upstream;
    UpstreamConfig backend;
    HealthCheckConfig health;
};

// One bounded GET/header transaction, with ONE deadline spanning all transport stages.
class Probe : public std::enable_shared_from_this<Probe>
{
   public:
    using Completion = std::function<void(bool, const char*)>;
    Probe(const asio::any_io_executor& executor, const ProbeTarget& target,
          std::shared_ptr<UpstreamTlsContext> context, Completion completion)
        : context_(std::move(context)),
          resolver_(executor),
          stream_(executor),
          deadline_(executor),
          backend_(target.backend),
          timeout_(target.health.timeout_ms),
          completion_(std::move(completion)),
          request_(http::verb::get, target.health.path, 11)
    {
        if (!completion_)
        {
            throw std::invalid_argument("health probe completion must not be empty");
        }
        if (backend_.scheme == UpstreamScheme::https)
        {
            if (!context_)
            {
                throw std::invalid_argument("HTTPS probe requires validated trust");
            }
            tls_ = std::make_unique<TlsStream>(std::move(stream_), context_->context);
        }
        request_.set(http::field::host, make_upstream_authority(backend_.host, backend_.port));
        request_.set(http::field::connection, "close");
        parser_.header_limit(std::uint32_t{16} * 1024);
        parser_.skip(true);  // Only the final header/status is inspected, never the body.
    }

    void start() noexcept
    {
        guarded(
            [this]
            {
                deadline_.expires_after(std::chrono::milliseconds{timeout_});
                deadline_.async_wait(
                    [self = shared_from_this()](const boost::system::error_code& error) noexcept
                    {
                        self->guarded(
                            [&]
                            {
                                if (!error)
                                {
                                    self->finish(false, "timeout");
                                }
                                else if (error != asio::error::operation_aborted)
                                {
                                    self->finish(false, "timer");
                                }
                            });
                    });
                resolver_.async_resolve(
                    backend_.host, std::to_string(backend_.port),
                    [self = shared_from_this()](const boost::system::error_code& error,
                                                const tcp::resolver::results_type& results) noexcept
                    {
                        self->guarded(
                            [&]
                            {
                                if (error)
                                {
                                    self->finish(false, "resolve");
                                    return;
                                }
                                self->transport().async_connect(
                                    results,
                                    [self](const boost::system::error_code& connect_error,
                                           const auto&) noexcept
                                    {
                                        self->guarded(
                                            [&]
                                            {
                                                self->connected(connect_error);
                                            });
                                    });
                            });
                    });
            });
    }

    void cancel() noexcept
    {
        if (done_)
        {
            return;
        }
        done_ = true;
        completion_ = nullptr;
        close();
    }

   private:
    using TlsStream = beast::ssl_stream<beast::tcp_stream>;
    beast::tcp_stream& transport() noexcept
    {
        return tls_ ? beast::get_lowest_layer(*tls_) : stream_;
    }
    template <typename Action>
    void with_stream(Action&& action)
    {
        if (tls_)
        {
            std::forward<Action>(action)(*tls_);
        }
        else
        {
            std::forward<Action>(action)(stream_);
        }
    }
    template <typename Action>
    void guarded(Action&& action) noexcept
    {
        if (done_)
        {
            return;
        }
        try
        {
            std::forward<Action>(action)();
        }
        catch (...)
        {
            finish(false, "exception");
        }
    }
    void connected(const boost::system::error_code& error)
    {
        if (error)
        {
            finish(false, "connect");
            return;
        }
        if (!tls_)
        {
            write();
            return;
        }
        if (!configure_upstream_tls_identity(*tls_, backend_.host))
        {
            finish(false, "handshake");
            return;
        }
        tls_->async_handshake(
            asio::ssl::stream_base::client,
            [self = shared_from_this()](const boost::system::error_code& handshake_error) noexcept
            {
                self->guarded(
                    [&]
                    {
                        if (handshake_error)
                        {
                            self->finish(false, "handshake");
                        }
                        else
                        {
                            self->write();
                        }
                    });
            });
    }
    void write()
    {
        with_stream(
            [this](auto& stream)
            {
                http::async_write(stream, request_,
                                  [self = shared_from_this()](
                                      const boost::system::error_code& error, std::size_t) noexcept
                                  {
                                      self->guarded(
                                          [&]
                                          {
                                              if (error)
                                              {
                                                  self->finish(false, "write");
                                                  return;
                                              }
                                              self->read_header();
                                          });
                                  });
            });
    }
    void read_header()
    {
        with_stream(
            [this](auto& stream)
            {
                http::async_read_header(
                    stream, buffer_, parser_,
                    [self = shared_from_this()](const boost::system::error_code& error,
                                                std::size_t) noexcept
                    {
                        self->guarded(
                            [&]
                            {
                                if (error)
                                {
                                    self->finish(false, "read");
                                    return;
                                }
                                const auto status = self->parser_.get().result_int();
                                self->finish(status >= 200U && status <= 299U, "status");
                            });
                    });
            });
    }
    void finish(bool success, const char* reason) noexcept
    {
        if (done_)
        {
            return;
        }
        done_ = true;
        close();
        auto callback = std::move(completion_);
        if (callback)
        {
            try
            {
                callback(success, success ? "probe_succeeded" : reason);
            }
            catch (...)
            {
                diagnostic("http_health_check_completion_failed");
            }
        }
    }
    void close() noexcept
    {
        try
        {
            resolver_.cancel();
        }
        catch (...)
        {
            diagnostic("http_health_check_resolver_cancel_failed");
        }
        try
        {
            static_cast<void>(deadline_.cancel());
        }
        catch (...)
        {
            diagnostic("http_health_check_timer_cancel_failed");
        }
        boost::system::error_code error;
        // Closing the lowest socket cancels connect/handshake/read/write; never await close_notify.
        if (transport().socket().is_open())
        {
            const auto close_error = transport().socket().close(error);
            if (close_error)
            {
                diagnostic("http_health_check_socket_close_failed");
            }
        }
    }

    std::shared_ptr<UpstreamTlsContext> context_;
    tcp::resolver resolver_;
    beast::tcp_stream stream_;
    std::unique_ptr<TlsStream> tls_;
    asio::steady_timer deadline_;
    UpstreamConfig backend_;
    std::uint32_t timeout_;
    Completion completion_;
    http::request<http::empty_body> request_;
    beast::flat_buffer buffer_{std::size_t{32} * 1024};
    http::response_parser<http::empty_body> parser_;
    bool done_{false};
};

}  // namespace

class HealthChecks::Impl : public std::enable_shared_from_this<HealthChecks::Impl>
{
   public:
    Impl(const asio::any_io_executor& executor, const AppConfig& config,
         std::shared_ptr<UpstreamHealthState> state, StructuredLogger& logger,
         std::function<void()> on_failure)
        : timer_(executor),
          context_(config.upstream_tls_context),
          state_(std::move(state)),
          logger_(logger),
          on_failure_(std::move(on_failure)),
          schedule_({}, Clock::now())
    {
        if (!state_ || !on_failure_)
        {
            throw std::invalid_argument("health check callbacks/state must not be empty");
        }
        std::vector<std::chrono::milliseconds> intervals;
        for (std::size_t route = 0; route < config.routes.size(); ++route)
        {
            const auto& entry = config.routes[route];
            if (!entry.health_check)
            {
                continue;
            }
            const auto& settings = *entry.health_check;
            for (std::size_t upstream = 0; upstream < entry.upstreams.size(); ++upstream)
            {
                targets_.push_back({route, upstream, entry.upstreams[upstream], settings});
                intervals.emplace_back(settings.interval_ms);
            }
        }
        probes_.resize(targets_.size());
        schedule_ = ProbeSchedule{intervals, Clock::now()};
    }
    void start()
    {
        if (started_ || stopped_ || targets_.empty())
        {
            return;
        }
        started_ = true;
        asio::post(timer_.get_executor(),
                   [self = shared_from_this()]() noexcept
                   {
                       self->pump();
                   });
    }
    void stop() noexcept
    {
        if (stopped_)
        {
            return;
        }
        stopped_ = true;
        schedule_.stop();
        try
        {
            static_cast<void>(timer_.cancel());
        }
        catch (...)
        {
            diagnostic("http_health_check_schedule_cancel_failed");
        }
        for (auto& probe : probes_)
        {
            if (probe)
            {
                probe->cancel();
                probe.reset();
            }
        }
    }

   private:
    void failed() noexcept
    {
        if (stopped_)
        {
            return;
        }
        stop();
        try
        {
            on_failure_();
        }
        catch (...)
        {
            diagnostic("http_health_check_failure_callback_failed");
        }
    }
    void completed(std::size_t index, bool success, const char* reason) noexcept
    {
        if (stopped_)
        {
            return;
        }
        try
        {
            schedule_.complete(index);
            probes_[index].reset();
            const auto& target = targets_[index];
            const auto transition = state_->record(target.route, target.upstream, success);
            if (transition != HealthTransition::unchanged)
            {
                try
                {
                    logger_.info(transition == HealthTransition::unhealthy
                                     ? "http_upstream_marked_unhealthy"
                                     : "http_upstream_marked_healthy",
                                 {{"route_index", target.route},
                                  {"upstream_index", target.upstream},
                                  {"reason", reason}});
                }
                catch (...)
                {
                    diagnostic("http_health_check_transition_log_failed");
                }
            }
            pump();
        }
        catch (...)
        {
            failed();
        }
    }
    void pump() noexcept
    {
        if (stopped_ || pumping_)
        {
            return;
        }
        pumping_ = true;
        try
        {
            while (const auto index = schedule_.take_due(Clock::now()))
            {
                const auto slot = *index;
                auto probe = std::make_shared<Probe>(
                    timer_.get_executor(), targets_[slot], context_,
                    [weak = weak_from_this(), slot](bool success, const char* reason) noexcept
                    {
                        if (const auto self = weak.lock())
                        {
                            self->completed(slot, success, reason);
                        }
                    });
                probes_[slot] = probe;
                probe->start();
            }
            if (const auto due = schedule_.next_due())
            {
                timer_.expires_at(*due);
                timer_.async_wait(
                    [self = shared_from_this()](const boost::system::error_code& error) noexcept
                    {
                        if (!error)
                        {
                            self->pump();
                        }
                        else if (error != asio::error::operation_aborted)
                        {
                            self->failed();
                        }
                    });
            }
        }
        catch (...)
        {
            failed();
        }
        pumping_ = false;
    }

    asio::steady_timer timer_;
    std::shared_ptr<UpstreamTlsContext> context_;
    std::shared_ptr<UpstreamHealthState> state_;
    StructuredLogger& logger_;
    std::function<void()> on_failure_;
    std::vector<ProbeTarget> targets_;
    std::vector<std::shared_ptr<Probe>> probes_;
    ProbeSchedule schedule_;
    bool started_{false};
    bool stopped_{false};
    bool pumping_{false};
};

HealthChecks::HealthChecks(const asio::any_io_executor& executor, const AppConfig& config,
                           std::shared_ptr<UpstreamHealthState> state, StructuredLogger& logger,
                           std::function<void()> on_failure)
    : impl_(
          std::make_shared<Impl>(executor, config, std::move(state), logger, std::move(on_failure)))
{
}
HealthChecks::~HealthChecks()
{
    stop();
}
void HealthChecks::start()
{
    impl_->start();
}
void HealthChecks::stop() noexcept
{
    impl_->stop();
}

}  // namespace apigate::detail
