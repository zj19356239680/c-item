#include "http_proxy.hpp"

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "upstream_tls.hpp"

namespace apigate
{
namespace
{

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::uint64_t max_upstream_body_bytes = std::uint64_t{1024} * 1024;
constexpr std::size_t max_upstream_header_bytes = std::size_t{16} * 1024;

void write_proxy_fallback_diagnostic(const char* event, int error_code) noexcept
{
    if (std::fprintf(stderr, R"({"level":"error","payload":{"event":"%s","error_code":%d}}%c)",
                     event, error_code, 10) < 0)
    {
        std::clearerr(stderr);
    }
}

[[nodiscard]] std::string lowercase(std::string_view value)
{
    std::string result{value};
    std::ranges::transform(result, result.begin(),
                           [](unsigned char character)
                           {
                               if (character >= 'A' && character <= 'Z')
                               {
                                   return static_cast<char>(character - 'A' + 'a');
                               }
                               return static_cast<char>(character);
                           });
    return result;
}

[[nodiscard]] std::string trim_http_token(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return lowercase(value.substr(first, last - first + 1));
}

[[nodiscard]] std::set<std::string> connection_tokens(const http::fields& fields)
{
    std::set<std::string> tokens;
    for (const auto& field : fields)
    {
        if (field.name() != http::field::connection)
        {
            continue;
        }
        const std::string_view value{field.value().data(), field.value().size()};
        std::size_t begin = 0;
        while (begin <= value.size())
        {
            const auto end = value.find(',', begin);
            const auto token = trim_http_token(value.substr(begin, end - begin));
            if (!token.empty())
            {
                tokens.insert(token);
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            begin = end + 1;
        }
    }
    return tokens;
}

[[nodiscard]] bool is_hop_by_hop(std::string_view name, const std::set<std::string>& dynamic_tokens)
{
    const auto normalized = lowercase(name);
    return normalized == "connection" || normalized == "keep-alive" || normalized == "te" ||
           normalized == "trailer" || normalized == "transfer-encoding" ||
           normalized == "upgrade" || normalized == "proxy-connection" ||
           normalized == "proxy-authenticate" || normalized == "proxy-authorization" ||
           dynamic_tokens.contains(normalized);
}

[[nodiscard]] bool is_forwarding_metadata(std::string_view name)
{
    return lowercase(name).starts_with("x-forwarded-");
}

}  // namespace

std::string_view to_string(ProxyFailureStage stage) noexcept
{
    switch (stage)
    {
        case ProxyFailureStage::resolve:
            return "resolve";
        case ProxyFailureStage::connect:
            return "connect";
        case ProxyFailureStage::handshake:
            return "handshake";
        case ProxyFailureStage::write:
            return "write";
        case ProxyFailureStage::read:
            return "read";
        case ProxyFailureStage::timeout:
            return "timeout";
        case ProxyFailureStage::response_too_large:
            return "response_too_large";
        case ProxyFailureStage::invalid_response:
            return "invalid_response";
    }
    return "read";
}

namespace detail
{

bool ProxyCompletionState::claim() noexcept
{
    if (completed_)
    {
        return false;
    }
    completed_ = true;
    return true;
}

std::string make_upstream_authority(std::string_view host, std::uint16_t port)
{
    const bool ipv6_literal = host.find(':') != std::string_view::npos;
    const std::string port_text = std::to_string(port);
    if (ipv6_literal)
    {
        return '[' + std::string{host} + "]:" + port_text;
    }
    return std::string{host} + ':' + port_text;
}

HttpRequest make_upstream_request(const SelectedUpstream& selected, const HttpRequest& request)
{
    const auto& config = selected.upstream;
    std::string_view target{request.target().data(), request.target().size()};
    std::string rewritten_target;
    if (selected.strip_prefix && !selected.prefix.empty() && selected.prefix != "/")
    {
        // Selection already established the raw path boundary. Do not decode or select again.
        target.remove_prefix(selected.prefix.size());
        if (target.empty() || target.front() == '?')
        {
            rewritten_target = '/';
            rewritten_target.append(target);
            target = rewritten_target;
        }
    }
    HttpRequest upstream{request.method(), target, 11};
    const auto dynamic_tokens = connection_tokens(request.base());
    for (const auto& field : request.base())
    {
        const std::string_view name{field.name_string().data(), field.name_string().size()};
        if (is_hop_by_hop(name, dynamic_tokens) || is_forwarding_metadata(name) ||
            field.name() == http::field::host || field.name() == http::field::content_length ||
            field.name() == http::field::expect)
        {
            continue;
        }
        upstream.insert(field.name_string(), field.value());
    }
    upstream.set(http::field::host, make_upstream_authority(config.host, config.port));
    upstream.set(http::field::connection, "close");
    upstream.body() = request.body();
    if (request.method() != http::verb::get)
    {
        upstream.content_length(upstream.body().size());
    }
    else if (!request.body().empty())
    {
        upstream.prepare_payload();
    }
    return upstream;
}

HttpResponse make_downstream_response(const HttpRequest& request, HttpResponse upstream_response)
{
    HttpResponse response{upstream_response.result(), request.version()};
    response.reason(upstream_response.reason());
    const auto dynamic_tokens = connection_tokens(upstream_response.base());
    for (const auto& field : upstream_response.base())
    {
        const std::string_view name{field.name_string().data(), field.name_string().size()};
        if (is_hop_by_hop(name, dynamic_tokens) || field.name() == http::field::server ||
            field.name() == http::field::content_length)
        {
            continue;
        }
        response.insert(field.name_string(), field.value());
    }
    response.set(http::field::server, "ApiGate");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(upstream_response.body());
    response.prepare_payload();
    return response;
}

}  // namespace detail

class HttpProxyExchange::Impl : public std::enable_shared_from_this<HttpProxyExchange::Impl>
{
   public:
    Impl(const asio::any_io_executor& executor, const SelectedUpstream& selected,
         const HttpRequest& request, std::shared_ptr<UpstreamTlsContext> tls_context,
         std::shared_ptr<detail::BufferBudgetState> buffer_budget,
         std::shared_ptr<detail::BufferBudgetPermit> request_body_permit,
         CompletionCallback on_complete, AbortCallback on_abort)
        : tls_context_(std::move(tls_context)),
          resolver_(executor),
          stream_(executor),
          deadline_(executor),
          config_(selected.upstream),
          downstream_request_(request),
          upstream_request_(detail::make_upstream_request(selected, request)),
          buffer_budget_(std::move(buffer_budget)),
          request_body_permit_(std::move(request_body_permit)),
          on_complete_(std::move(on_complete)),
          on_abort_(std::move(on_abort))
    {
        if (!buffer_budget_)
        {
            throw std::invalid_argument("buffer budget must not be empty");
        }
        if (!on_complete_)
        {
            throw std::invalid_argument("proxy completion callback must not be empty");
        }
        if (!on_abort_)
        {
            throw std::invalid_argument("proxy abort callback must not be empty");
        }
        if (config_.scheme == UpstreamScheme::https)
        {
            if (!tls_context_)
            {
                throw std::invalid_argument("HTTPS requires a validated TLS context");
            }
            tls_stream_ = std::make_unique<TlsStream>(std::move(stream_), tls_context_->context);
        }
        response_parser_.body_limit(max_upstream_body_bytes);
        response_parser_.header_limit(max_upstream_header_bytes);
    }

    void start()
    {
        begin_deadline();
        resolver_.async_resolve(
            config_.host, std::to_string(config_.port),
            [self = shared_from_this()](const boost::system::error_code& error,
                                        const tcp::resolver::results_type& results) noexcept
            {
                self->run_handler(
                    [self, &error, &results]
                    {
                        self->on_resolve(error, results);
                    });
            });
    }

    void cancel() noexcept
    {
        if (!completion_state_.claim())
        {
            return;
        }
        on_complete_ = nullptr;
        on_abort_ = nullptr;
        cancel_operations();
    }

   private:
    using TlsStream = beast::ssl_stream<beast::tcp_stream>;

    [[nodiscard]] beast::tcp_stream& transport() noexcept
    {
        return tls_stream_ ? beast::get_lowest_layer(*tls_stream_) : stream_;
    }

    template <typename Action>
    void with_stream(Action&& action)
    {
        if (tls_stream_)
        {
            std::forward<Action>(action)(*tls_stream_);
        }
        else
        {
            std::forward<Action>(action)(stream_);
        }
    }

    void begin_deadline()
    {
        ++deadline_generation_;
        const auto generation = deadline_generation_;
        deadline_.expires_after(std::chrono::milliseconds{config_.timeout_ms});
        deadline_.async_wait(
            [self = shared_from_this(), generation](const boost::system::error_code& error) noexcept
            {
                self->run_handler(
                    [self, generation, &error]
                    {
                        if (!error && generation == self->deadline_generation_)
                        {
                            self->fail(ProxyFailureStage::timeout, asio::error::timed_out);
                        }
                    });
            });
    }

    void finish_deadline() noexcept
    {
        ++deadline_generation_;
        try
        {
            static_cast<void>(deadline_.cancel());
        }
        catch (const boost::system::system_error& error)
        {
            write_proxy_fallback_diagnostic("http_proxy_timer_cancel_failed", error.code().value());
        }
        catch (...)
        {
            write_proxy_fallback_diagnostic("http_proxy_timer_cancel_failed", 0);
        }
    }

    void on_resolve(const boost::system::error_code& error,
                    const tcp::resolver::results_type& results)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        if (error)
        {
            fail(ProxyFailureStage::resolve, error);
            return;
        }
        begin_deadline();
        transport().async_connect(
            results,
            [self = shared_from_this()](const boost::system::error_code& connect_error,
                                        const tcp::resolver::results_type::endpoint_type&) noexcept
            {
                self->run_handler(
                    [self, &connect_error]
                    {
                        self->on_connect(connect_error);
                    });
            });
    }

    void on_connect(const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        if (error)
        {
            fail(ProxyFailureStage::connect, error);
            return;
        }
        if (tls_stream_)
        {
            if (!configure_upstream_tls_identity(*tls_stream_, config_.host))
            {
                fail(ProxyFailureStage::handshake, asio::error::invalid_argument);
                return;
            }
            begin_deadline();
            tls_stream_->async_handshake(
                asio::ssl::stream_base::client,
                [self =
                     shared_from_this()](const boost::system::error_code& handshake_error) noexcept
                {
                    self->run_handler(
                        [self, &handshake_error]
                        {
                            self->on_handshake(handshake_error);
                        });
                });
            return;
        }
        write_request();
    }

    void on_handshake(const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        if (error)
        {
            fail(ProxyFailureStage::handshake, error);
            return;
        }
        write_request();
    }

    void write_request()
    {
        begin_deadline();
        with_stream(
            [&](auto& stream)
            {
                http::async_write(
                    stream, upstream_request_,
                    [self = shared_from_this()](const boost::system::error_code& write_error,
                                                std::size_t) noexcept
                    {
                        self->run_handler(
                            [self, &write_error]
                            {
                                self->on_write(write_error);
                            });
                    });
            });
    }

    void on_write(const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        if (error)
        {
            fail(ProxyFailureStage::write, error);
            return;
        }
        begin_deadline();
        with_stream(
            [&](auto& stream)
            {
                http::async_read_header(
                    stream, buffer_, response_parser_,
                    [self = shared_from_this()](const boost::system::error_code& read_error,
                                                std::size_t) noexcept
                    {
                        self->run_handler(
                            [self, &read_error]
                            {
                                self->on_read_header(read_error);
                            });
                    });
            });
    }

    void on_read_header(const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        if (error)
        {
            finish_deadline();
            const auto stage = error == http::error::body_limit
                                   ? ProxyFailureStage::response_too_large
                                   : ProxyFailureStage::read;
            fail(stage, error);
            return;
        }
        if (response_parser_.get().result_int() < 200U)
        {
            finish_deadline();
            fail(ProxyFailureStage::invalid_response, {});
            return;
        }

        const auto content_length = response_parser_.content_length();
        const std::optional<std::uint64_t> planned_content_length =
            content_length ? std::optional<std::uint64_t>{*content_length} : std::nullopt;
        const auto plan = detail::plan_body_reservation(
            planned_content_length, response_parser_.is_done(), max_upstream_body_bytes);
        if (plan.kind == detail::BodyReservationKind::too_large)
        {
            finish_deadline();
            fail(ProxyFailureStage::response_too_large, http::error::body_limit);
            return;
        }
        if (plan.kind == detail::BodyReservationKind::none)
        {
            finish_deadline();
            finish_response();
            return;
        }

        auto permit = buffer_budget_->try_acquire(plan.bytes);
        if (!permit)
        {
            finish_deadline();
            reject_buffer_budget(plan.bytes);
            return;
        }
        response_body_permit_ = std::move(*permit);
        with_stream(
            [&](auto& stream)
            {
                http::async_read(
                    stream, buffer_, response_parser_,
                    [self = shared_from_this()](const boost::system::error_code& read_error,
                                                std::size_t) noexcept
                    {
                        self->run_handler(
                            [self, &read_error]
                            {
                                self->on_read_body(read_error);
                            });
                    });
            });
    }

    void on_read_body(const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        if (error)
        {
            const auto stage = error == http::error::body_limit
                                   ? ProxyFailureStage::response_too_large
                                   : ProxyFailureStage::read;
            fail(stage, error);
            return;
        }
        const auto actual_bytes = static_cast<std::uint64_t>(response_parser_.get().body().size());
        if (!response_body_permit_.shrink_to(actual_bytes))
        {
            fail(ProxyFailureStage::read, {});
            return;
        }
        finish_response();
    }

    void finish_response()
    {
        auto upstream_response = response_parser_.release();
        close_upstream();
        auto response =
            detail::make_downstream_response(downstream_request_, std::move(upstream_response));
        ProxyResult result{std::move(response), std::nullopt, std::nullopt,
                           std::move(response_body_permit_)};
        complete(std::move(result));
    }

    void reject_buffer_budget(std::uint64_t requested_bytes)
    {
        cancel_operations();
        auto response = make_gateway_error_response(downstream_request_,
                                                    GatewayFailure::buffer_capacity_exhausted);
        complete(ProxyResult{std::move(response), std::nullopt, requested_bytes, {}});
    }

    void fail(ProxyFailureStage stage, const boost::system::error_code& error)
    {
        if (completion_claimed())
        {
            return;
        }
        finish_deadline();
        cancel_operations();
        const auto failure = stage == ProxyFailureStage::timeout ? GatewayFailure::gateway_timeout
                                                                 : GatewayFailure::bad_gateway;
        auto response = make_gateway_error_response(downstream_request_, failure);
        complete(
            ProxyResult{std::move(response), ProxyFailure{stage, error.value()}, std::nullopt, {}});
    }

    [[nodiscard]] bool completion_claimed() const noexcept
    {
        return completion_observed_;
    }

    template <typename HandlerAction>
    void run_handler(HandlerAction&& handler) noexcept
    {
        detail::run_proxy_handler_action(
            completion_state_, std::forward<HandlerAction>(handler),
            [this]() noexcept
            {
                cancel_operations();
            },
            [this]() noexcept
            {
                write_proxy_fallback_diagnostic("http_proxy_handler_exception", 0);
                notify_abort();
            });
    }

    void complete(ProxyResult result)
    {
        detail::run_proxy_completion_action(
            completion_state_,
            [this, &result]
            {
                completion_observed_ = true;
                auto callback = std::move(on_complete_);
                if (!callback)
                {
                    notify_abort();
                    return;
                }
                callback(std::move(result));
                on_abort_ = nullptr;
            },
            [this]() noexcept
            {
                completion_observed_ = true;
                write_proxy_fallback_diagnostic("http_proxy_completion_exception", 0);
                notify_abort();
            });
    }

    void notify_abort() noexcept
    {
        on_complete_ = nullptr;
        auto callback = std::move(on_abort_);
        if (!callback)
        {
            return;
        }
        try
        {
            callback();
        }
        catch (...)
        {
            write_proxy_fallback_diagnostic("http_proxy_abort_callback_exception", 0);
        }
    }

    void cancel_operations() noexcept
    {
        completion_observed_ = true;
        try
        {
            resolver_.cancel();
        }
        catch (const boost::system::system_error& error)
        {
            write_proxy_fallback_diagnostic("http_proxy_resolver_cancel_failed",
                                            error.code().value());
        }
        catch (...)
        {
            write_proxy_fallback_diagnostic("http_proxy_resolver_cancel_failed", 0);
        }
        try
        {
            static_cast<void>(deadline_.cancel());
        }
        catch (const boost::system::system_error& error)
        {
            write_proxy_fallback_diagnostic("http_proxy_timer_cancel_failed", error.code().value());
        }
        catch (...)
        {
            write_proxy_fallback_diagnostic("http_proxy_timer_cancel_failed", 0);
        }
        close_upstream();
    }

    void close_upstream() noexcept
    {
        // Each exchange closes its connection. Once HTTP framing is complete, do not wait
        // for TLS close_notify. Before completion, TLS truncation remains a read failure.
        auto& socket = transport().socket();
        if (!socket.is_open())
        {
            return;
        }
        boost::system::error_code operation_error;
        const auto cancel_error = socket.cancel(operation_error);
        if (cancel_error && cancel_error != asio::error::not_connected)
        {
            write_proxy_fallback_diagnostic("http_proxy_socket_cancel_failed",
                                            cancel_error.value());
        }
        operation_error.clear();
        const auto shutdown_error = socket.shutdown(tcp::socket::shutdown_both, operation_error);
        if (shutdown_error && shutdown_error != asio::error::not_connected)
        {
            write_proxy_fallback_diagnostic("http_proxy_socket_shutdown_failed",
                                            shutdown_error.value());
        }
        operation_error.clear();
        const auto close_error = socket.close(operation_error);
        if (close_error)
        {
            write_proxy_fallback_diagnostic("http_proxy_socket_close_failed", close_error.value());
        }
    }

    std::shared_ptr<UpstreamTlsContext> tls_context_;
    tcp::resolver resolver_;
    beast::tcp_stream stream_;
    std::unique_ptr<TlsStream> tls_stream_;
    asio::steady_timer deadline_;
    UpstreamConfig config_;
    HttpRequest downstream_request_;
    HttpRequest upstream_request_;
    std::shared_ptr<detail::BufferBudgetState> buffer_budget_;
    std::shared_ptr<detail::BufferBudgetPermit> request_body_permit_;
    beast::flat_buffer buffer_{max_upstream_header_bytes};
    http::response_parser<http::string_body> response_parser_;
    detail::BufferBudgetPermit response_body_permit_;
    CompletionCallback on_complete_;
    AbortCallback on_abort_;
    detail::ProxyCompletionState completion_state_;
    std::uint64_t deadline_generation_{0};
    bool completion_observed_{false};
};

HttpProxyExchange::HttpProxyExchange(
    const asio::any_io_executor& executor, const SelectedUpstream& selected,
    const HttpRequest& request, std::shared_ptr<UpstreamTlsContext> tls_context,
    std::shared_ptr<detail::BufferBudgetState> buffer_budget,
    std::shared_ptr<detail::BufferBudgetPermit> request_body_permit, CompletionCallback on_complete,
    AbortCallback on_abort)
    // Boost.Asio's resolver service calls its final shutdown hook during service destruction;
    // the analyzer reports this library-owned, non-polymorphic cleanup as virtual dispatch.
    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
    : impl_(std::make_shared<Impl>(executor, selected, request, std::move(tls_context),
                                   std::move(buffer_budget), std::move(request_body_permit),
                                   std::move(on_complete), std::move(on_abort)))
{
}

HttpProxyExchange::~HttpProxyExchange() = default;

void HttpProxyExchange::start()
{
    impl_->start();
}

void HttpProxyExchange::cancel() noexcept
{
    impl_->cancel();
}

}  // namespace apigate
