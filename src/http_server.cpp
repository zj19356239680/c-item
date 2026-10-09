#include "apigate/http_server.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "apigate/config.hpp"
#include "apigate/http_handler.hpp"
#include "apigate/logging.hpp"
#include "http_proxy.hpp"
#include "http_server_internal.hpp"

namespace apigate {

namespace detail {

HttpWriteDisposition classify_http_write_result(const boost::system::error_code& error,
                                                bool stopping) noexcept {
    if (stopping || error == boost::asio::error::operation_aborted) {
        return HttpWriteDisposition::expected_cancellation;
    }
    if (!error) {
        return HttpWriteDisposition::success;
    }
    if (error == boost::asio::error::broken_pipe ||
        error == boost::asio::error::connection_aborted ||
        error == boost::asio::error::connection_reset || error == boost::asio::error::eof) {
        return HttpWriteDisposition::client_disconnected;
    }
    return HttpWriteDisposition::unexpected_error;
}

AcceptRetryDisposition classify_accept_retry_completion(const boost::system::error_code& error,
                                                        bool stopping) noexcept {
    if (stopping) {
        return AcceptRetryDisposition::stopped;
    }
    if (error) {
        return AcceptRetryDisposition::fatal;
    }
    return AcceptRetryDisposition::retry;
}

AcceptCapacityState::AcceptCapacityState(std::size_t max_connections) noexcept
    : max_connections_(max_connections) {}

AcceptCapacityAction AcceptCapacityState::next_action() noexcept {
    if (stopped_ || accept_pending_ || retry_pending_) {
        return AcceptCapacityAction::none;
    }
    if (active_connections_ >= max_connections_) {
        if (paused_) {
            return AcceptCapacityAction::none;
        }
        paused_ = true;
        return AcceptCapacityAction::pause;
    }

    accept_pending_ = true;
    if (paused_) {
        paused_ = false;
        return AcceptCapacityAction::resume_and_start;
    }
    return AcceptCapacityAction::start_accept;
}

void AcceptCapacityState::accept_completed() noexcept { accept_pending_ = false; }

void AcceptCapacityState::retry_scheduled() noexcept { retry_pending_ = true; }

void AcceptCapacityState::retry_completed() noexcept { retry_pending_ = false; }

void AcceptCapacityState::session_started() noexcept { ++active_connections_; }

void AcceptCapacityState::session_finished() noexcept {
    if (active_connections_ > 0) {
        --active_connections_;
    }
}

void AcceptCapacityState::stop() noexcept { stopped_ = true; }

std::size_t AcceptCapacityState::active_connections() const noexcept { return active_connections_; }

std::size_t AcceptCapacityState::max_connections() const noexcept { return max_connections_; }

ProxyPermit::ProxyPermit(std::shared_ptr<ProxyAdmissionState> state) noexcept
    : state_(std::move(state)) {}

std::optional<ProxyPermit> ProxyPermit::try_acquire(
    std::shared_ptr<ProxyAdmissionState> state) noexcept {
    if (!state || state->active_proxies_ >= state->max_proxies_) {
        return std::nullopt;
    }
    ++state->active_proxies_;
    return ProxyPermit{std::move(state)};
}

ProxyPermit::~ProxyPermit() { release(); }

ProxyPermit::ProxyPermit(ProxyPermit&& other) noexcept : state_(std::move(other.state_)) {}

ProxyPermit& ProxyPermit::operator=(ProxyPermit&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

void ProxyPermit::release() noexcept {
    if (!state_) {
        return;
    }
    auto state = std::move(state_);
    state->release();
}

bool ProxyPermit::owns_capacity() const noexcept { return state_ != nullptr; }

ProxyAdmissionState::ProxyAdmissionState(std::size_t max_proxies) noexcept
    : max_proxies_(max_proxies) {}

std::size_t ProxyAdmissionState::active_proxies() const noexcept { return active_proxies_; }

std::size_t ProxyAdmissionState::max_proxies() const noexcept { return max_proxies_; }

void ProxyAdmissionState::release() noexcept {
    if (active_proxies_ > 0) {
        --active_proxies_;
    }
}

}  // namespace detail

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::size_t max_header_bytes = std::size_t{8} * 1024;
constexpr std::uint64_t max_body_bytes = std::uint64_t{64} * 1024;
constexpr std::size_t max_requests_per_connection = 100;
constexpr auto request_timeout = std::chrono::seconds{10};
constexpr auto accept_retry_delay = std::chrono::milliseconds{100};

[[nodiscard]] std::string_view proxy_method_name(http::verb method) noexcept {
    switch (method) {
        case http::verb::get:
            return "GET";
        case http::verb::post:
            return "POST";
        case http::verb::put:
            return "PUT";
        case http::verb::patch:
            return "PATCH";
        default:
            return "UNKNOWN";
    }
}

void write_fallback_diagnostic(const char* event, int error_code) noexcept {
    if (std::fprintf(stderr, R"({"level":"error","payload":{"event":"%s","error_code":%d}}%c)",
                     event, error_code, 10) < 0) {
        std::clearerr(stderr);
    }
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
   public:
    using CloseCallback = std::function<void(HttpSession*)>;

    HttpSession(tcp::socket socket, const AppConfig& config, StructuredLogger& logger,
                std::shared_ptr<detail::ProxyAdmissionState> proxy_admission,
                CloseCallback on_close)
        : stream_(std::move(socket)),
          config_(config),
          logger_(logger),
          proxy_admission_(std::move(proxy_admission)),
          on_close_(std::move(on_close)) {
        boost::system::error_code error;
        const auto endpoint = stream_.socket().remote_endpoint(error);
        remote_address_ = error ? "unknown" : endpoint.address().to_string();
    }

    void start() { read_request(); }

    void stop() noexcept {
        stopping_ = true;
        if (proxy_exchange_) {
            proxy_exchange_->cancel();
            proxy_exchange_.reset();
        }
        proxy_permit_.reset();
        boost::system::error_code operation_error;
        const auto cancel_error = stream_.socket().cancel(operation_error);
        if (cancel_error) {
            write_fallback_diagnostic("http_session_cancel_failed", cancel_error.value());
        }
        operation_error.clear();
        const auto shutdown_error =
            stream_.socket().shutdown(tcp::socket::shutdown_both, operation_error);
        if (shutdown_error && shutdown_error != asio::error::not_connected) {
            write_fallback_diagnostic("http_session_shutdown_failed", shutdown_error.value());
        }
        operation_error.clear();
        const auto close_error = stream_.socket().close(operation_error);
        if (close_error) {
            write_fallback_diagnostic("http_session_close_failed", close_error.value());
        }
        finish();
    }

   private:
    void read_request() {
        if (stopping_) {
            finish();
            return;
        }

        parser_.emplace();
        parser_->header_limit(max_header_bytes);
        // Inspect Expect before applying the payload limit so a client waiting for
        // 100-continue receives the promised 417 without sending its body.
        parser_->body_limit(std::numeric_limits<std::uint64_t>::max());
        stream_.expires_after(request_timeout);
        http::async_read_header(
            stream_, buffer_, *parser_,
            [self = shared_from_this()](const boost::system::error_code& error, std::size_t) {
                self->on_read_header(error);
            });
    }

    void on_read_header(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        if (error) {
            handle_read_error(error);
            return;
        }
        if (!parser_.has_value()) {
            logger_.error("http_parser_state_invalid");
            close_socket();
            return;
        }

        const auto& request = parser_->get();
        const auto [expect_begin, expect_end] = request.equal_range(http::field::expect);
        for (auto expect = expect_begin; expect != expect_end; ++expect) {
            if (!expect->value().empty()) {
                write_protocol_error(http::status::expectation_failed, "expectation_failed",
                                     "Expect is not supported");
                return;
            }
        }
        const auto content_length = parser_->content_length();
        if (content_length && *content_length > max_body_bytes) {
            write_protocol_error(http::status::payload_too_large, "payload_too_large",
                                 "request body is too large");
            return;
        }

        parser_->body_limit(max_body_bytes);
        if (parser_->is_done()) {
            process_request();
            return;
        }

        http::async_read(
            stream_, buffer_, *parser_,
            [self = shared_from_this()](const boost::system::error_code& body_error, std::size_t) {
                self->on_read_body(body_error);
            });
    }

    void on_read_body(const boost::system::error_code& error) {
        if (finished_) {
            return;
        }
        if (error) {
            handle_read_error(error);
            return;
        }
        process_request();
    }

    void process_request() {
        if (!parser_.has_value()) {
            logger_.error("http_parser_state_invalid");
            close_socket();
            return;
        }
        ++requests_served_;
        const auto& request = parser_->get();
        if (should_proxy_http_request(config_, request)) {
            start_proxy_request(request);
            return;
        }
        auto response = handle_http_request(config_, request);
        if (requests_served_ >= max_requests_per_connection) {
            response.keep_alive(false);
        }

        const std::string_view target{request.target().data(), request.target().size()};
        const std::string method{request.method_string().data(), request.method_string().size()};
        const std::string route{classify_http_route(target)};
        log_request_completed(method, route, response.result_int());
        write_response(std::move(response));
    }

    void handle_read_error(const boost::system::error_code& error) {
        if (error == http::error::end_of_stream || error == asio::error::operation_aborted) {
            close_socket();
            return;
        }
        if (error == http::error::body_limit) {
            write_protocol_error(http::status::payload_too_large, "payload_too_large",
                                 "request body is too large");
            return;
        }
        if (error == http::error::header_limit) {
            write_protocol_error(http::status::request_header_fields_too_large, "headers_too_large",
                                 "request headers are too large");
            return;
        }

        logger_.warn("http_request_rejected", {{"error_code", error.value()}});
        write_protocol_error(http::status::bad_request, "bad_request", "malformed HTTP request");
    }

    void write_protocol_error(http::status status, std::string_view code,
                              std::string_view message) {
        HttpResponse response{status, 11};
        response.set(http::field::server, "ApiGate");
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.keep_alive(false);
        response.body() =
            nlohmann::json{
                {"error", {{"code", std::string{code}}, {"message", std::string{message}}}}}
                .dump();
        response.prepare_payload();
        logger_.warn("http_request_rejected",
                     {{"reason", std::string{code}}, {"status", response.result_int()}});
        write_response(std::move(response));
    }

    void start_proxy_request(const HttpRequest& request) noexcept {
        std::shared_ptr<HttpProxyExchange> exchange;
        const auto method = request.method();
        try {
            if (!config_.upstream.has_value()) {
                throw std::logic_error{"proxy request requires upstream configuration"};
            }
            proxy_permit_ = detail::ProxyPermit::try_acquire(proxy_admission_);
            if (!proxy_permit_) {
                auto response =
                    make_gateway_error_response(request, GatewayFailure::gateway_overloaded);
                if (requests_served_ >= max_requests_per_connection) {
                    response.keep_alive(false);
                }
                log_proxy_capacity_rejection(response.result_int());
                log_request_completed(proxy_method_name(method), "proxy", response.result_int());
                write_response(std::move(response));
                return;
            }
            exchange = std::make_shared<HttpProxyExchange>(
                stream_.get_executor(), config_.upstream.value(), request,
                [weak_self = weak_from_this(), method](ProxyResult result) noexcept {
                    if (const auto self = weak_self.lock()) {
                        self->on_proxy_complete(method, std::move(result));
                    }
                },
                [weak_self = weak_from_this()]() noexcept {
                    if (const auto self = weak_self.lock()) {
                        self->on_proxy_aborted();
                    }
                });
            proxy_exchange_ = exchange;
            exchange->start();
        } catch (...) {
            if (exchange) {
                exchange->cancel();
            }
            proxy_exchange_.reset();
            proxy_permit_.reset();
            try {
                auto response = make_gateway_error_response(request, GatewayFailure::bad_gateway);
                log_proxy_failure(ProxyFailure{ProxyFailureStage::connect, 0},
                                  response.result_int());
                log_request_completed(proxy_method_name(method), "proxy", response.result_int());
                write_response(std::move(response));
            } catch (...) {
                write_fallback_diagnostic("http_proxy_start_failed", 0);
                close_socket();
            }
        }
    }

    void on_proxy_complete(http::verb method, ProxyResult result) noexcept {
        try {
            proxy_exchange_.reset();
            proxy_permit_.reset();
            if (finished_ || stopping_) {
                return;
            }
            if (requests_served_ >= max_requests_per_connection) {
                result.response.keep_alive(false);
            }
            if (result.failure) {
                log_proxy_failure(*result.failure, result.response.result_int());
            }
            log_request_completed(proxy_method_name(method), "proxy", result.response.result_int());
            write_response(std::move(result.response));
        } catch (...) {
            write_fallback_diagnostic("http_proxy_completion_failed", 0);
            close_socket();
        }
    }

    void on_proxy_aborted() noexcept {
        proxy_exchange_.reset();
        proxy_permit_.reset();
        if (finished_ || stopping_) {
            return;
        }
        close_socket();
    }

    void log_proxy_failure(const ProxyFailure& failure, unsigned int status) noexcept {
        try {
            logger_.warn("http_upstream_request_failed", {{"stage", to_string(failure.stage)},
                                                          {"error_code", failure.error_code},
                                                          {"status", status}});
        } catch (...) {
            write_fallback_diagnostic("http_upstream_failure_log_failed", failure.error_code);
        }
    }

    void log_proxy_capacity_rejection(unsigned int status) noexcept {
        try {
            logger_.warn("http_proxy_rejected_capacity",
                         {{"active_proxies", proxy_admission_->active_proxies()},
                          {"max_proxies", proxy_admission_->max_proxies()},
                          {"status", status}});
        } catch (...) {
            write_fallback_diagnostic("http_proxy_capacity_log_failed", 0);
        }
    }

    void log_request_completed(std::string_view method, std::string_view route,
                               unsigned int status) noexcept {
        try {
            logger_.info("http_request_completed", {{"method", method},
                                                    {"route", route},
                                                    {"status", status},
                                                    {"remote_address", remote_address_}});
        } catch (...) {
            write_fallback_diagnostic("http_request_log_failed", 0);
        }
    }

    void write_response(HttpResponse response) {
        const bool close_after_write = response.need_eof();
        response_.emplace(std::move(response));
        stream_.expires_after(request_timeout);
        http::async_write(stream_, *response_,
                          [self = shared_from_this(), close_after_write](
                              const boost::system::error_code& error, std::size_t) {
                              self->on_write(error, close_after_write);
                          });
    }

    void on_write(const boost::system::error_code& error, bool close_after_write) {
        if (finished_) {
            return;
        }
        response_.reset();

        switch (detail::classify_http_write_result(error, stopping_)) {
            case detail::HttpWriteDisposition::success:
                if (close_after_write) {
                    close_socket();
                } else {
                    read_request();
                }
                return;
            case detail::HttpWriteDisposition::expected_cancellation:
            case detail::HttpWriteDisposition::client_disconnected:
                close_socket();
                return;
            case detail::HttpWriteDisposition::unexpected_error:
                logger_.error("http_response_write_failed", {{"error_code", error.value()}});
                close_socket();
                return;
        }
    }

    void close_socket() noexcept {
        if (finished_) {
            return;
        }
        boost::system::error_code operation_error;
        const auto shutdown_error =
            stream_.socket().shutdown(tcp::socket::shutdown_send, operation_error);
        if (shutdown_error && shutdown_error != asio::error::not_connected) {
            write_fallback_diagnostic("http_session_shutdown_failed", shutdown_error.value());
        }
        operation_error.clear();
        const auto close_error = stream_.socket().close(operation_error);
        if (close_error) {
            write_fallback_diagnostic("http_session_close_failed", close_error.value());
        }
        finish();
    }

    void finish() noexcept {
        if (finished_) {
            return;
        }
        finished_ = true;
        on_close_(this);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_{max_header_bytes + static_cast<std::size_t>(max_body_bytes)};
    const AppConfig& config_;
    StructuredLogger& logger_;
    std::shared_ptr<detail::ProxyAdmissionState> proxy_admission_;
    CloseCallback on_close_;
    std::optional<http::request_parser<http::string_body>> parser_;
    std::optional<HttpResponse> response_;
    std::shared_ptr<HttpProxyExchange> proxy_exchange_;
    std::optional<detail::ProxyPermit> proxy_permit_;
    std::string remote_address_;
    std::size_t requests_served_{0};
    bool stopping_{false};
    bool finished_{false};
};

}  // namespace

class HttpServer::Impl {
   public:
    Impl(asio::io_context& io_context, const AppConfig& config, StructuredLogger& logger,
         std::function<void()> on_runtime_failure)
        : config_(config),
          logger_(logger),
          acceptor_(io_context),
          accept_retry_timer_(io_context),
          accept_capacity_(config.max_connections),
          proxy_admission_(
              std::make_shared<detail::ProxyAdmissionState>(config.max_concurrent_proxies)),
          on_runtime_failure_(std::move(on_runtime_failure)) {
        if (!on_runtime_failure_) {
            throw std::invalid_argument("runtime failure callback must not be empty");
        }
    }

    ~Impl() { stop(); }

    void start() {
        if (started_) {
            return;
        }

        const auto address = asio::ip::make_address(config_.listen_address);
        const tcp::endpoint endpoint{address, config_.listen_port};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(asio::socket_base::max_listen_connections);
        bound_port_ = acceptor_.local_endpoint().port();
        started_ = true;

        logger_.info("http_listener_started",
                     {{"address", config_.listen_address}, {"port", bound_port_}});
        drive_accept();
    }

    void stop() noexcept {
        if (stopping_) {
            return;
        }
        stopping_ = true;
        accept_capacity_.stop();

        boost::system::error_code operation_error;
        try {
            const auto cancelled_operations = accept_retry_timer_.cancel();
            if (cancelled_operations > 1U) {
                write_fallback_diagnostic("http_accept_timer_invalid_cancel_count",
                                          static_cast<int>(cancelled_operations));
            }
        } catch (const boost::system::system_error& error) {
            write_fallback_diagnostic("http_accept_timer_cancel_failed", error.code().value());
            // Socket cleanup must continue even when the timer service reports an error.
        }
        const auto cancel_error = acceptor_.cancel(operation_error);
        if (cancel_error) {
            write_fallback_diagnostic("http_acceptor_cancel_failed", cancel_error.value());
        }
        operation_error.clear();
        const auto close_error = acceptor_.close(operation_error);
        if (close_error) {
            write_fallback_diagnostic("http_acceptor_close_failed", close_error.value());
        }

        const auto active_sessions = sessions_.size();
        decltype(sessions_) sessions;
        sessions.swap(sessions_);
        for (const auto& session : sessions) {
            session->stop();
        }
        if (started_) {
            try {
                logger_.info("http_listener_stopped", {{"active_sessions", active_sessions}});
            } catch (const std::exception&) {
                write_fallback_diagnostic("http_listener_stop_log_failed", 0);
            }
        }
    }

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

   private:
    void drive_accept() {
        switch (accept_capacity_.next_action()) {
            case detail::AcceptCapacityAction::none:
                return;
            case detail::AcceptCapacityAction::pause:
                log_accept_capacity("http_accept_paused_capacity");
                return;
            case detail::AcceptCapacityAction::resume_and_start:
                log_accept_capacity("http_accept_resumed_capacity");
                break;
            case detail::AcceptCapacityAction::start_accept:
                break;
        }

        acceptor_.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
            accept_capacity_.accept_completed();
            if (stopping_) {
                return;
            }
            if (error) {
                logger_.error("http_accept_failed", {{"error_code", error.value()}});
                accept_retry_timer_.expires_after(accept_retry_delay);
                accept_capacity_.retry_scheduled();
                accept_retry_timer_.async_wait(
                    [this](const boost::system::error_code& timer_error) {
                        on_accept_retry_timer(timer_error);
                    });
                return;
            }

            auto session = std::make_shared<HttpSession>(
                std::move(socket), config_, logger_, proxy_admission_,
                [this](HttpSession* closed_session) { remove_session(closed_session); });
            sessions_.insert(session);
            accept_capacity_.session_started();
            session->start();
            drive_accept();
        });
    }

    void on_accept_retry_timer(const boost::system::error_code& error) {
        accept_capacity_.retry_completed();
        switch (detail::classify_accept_retry_completion(error, stopping_)) {
            case detail::AcceptRetryDisposition::retry:
                drive_accept();
                return;
            case detail::AcceptRetryDisposition::stopped:
                return;
            case detail::AcceptRetryDisposition::fatal:
                fail_accept_retry(error);
                return;
        }
    }

    void log_accept_capacity(std::string_view event) noexcept {
        try {
            logger_.info(event, {{"active_connections", accept_capacity_.active_connections()},
                                 {"max_connections", accept_capacity_.max_connections()}});
        } catch (...) {
            write_fallback_diagnostic("http_accept_capacity_log_failed", 0);
        }
    }

    void fail_accept_retry(const boost::system::error_code& error) {
        detail::run_accept_retry_failure_actions(
            runtime_failure_state_,
            [this, &error] {
                logger_.critical("http_accept_retry_failed", {{"error_code", error.value()}});
            },
            [&error]() noexcept {
                write_fallback_diagnostic("http_accept_retry_failed", error.value());
            },
            [this]() noexcept { stop(); }, [this]() noexcept { on_runtime_failure_(); });
    }

    void remove_session(HttpSession* closed_session) noexcept {
        for (auto iterator = sessions_.begin(); iterator != sessions_.end(); ++iterator) {
            if (iterator->get() == closed_session) {
                sessions_.erase(iterator);
                accept_capacity_.session_finished();
                try {
                    drive_accept();
                } catch (...) {
                    if (runtime_failure_state_.claim()) {
                        write_fallback_diagnostic("http_accept_resume_failed", 0);
                        stop();
                        on_runtime_failure_();
                    }
                }
                return;
            }
        }
    }

    const AppConfig& config_;
    StructuredLogger& logger_;
    tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    detail::AcceptCapacityState accept_capacity_;
    std::shared_ptr<detail::ProxyAdmissionState> proxy_admission_;
    std::function<void()> on_runtime_failure_;
    detail::RuntimeFailureState runtime_failure_state_;
    std::set<std::shared_ptr<HttpSession>, std::owner_less<std::shared_ptr<HttpSession>>> sessions_;
    std::uint16_t bound_port_{0};
    bool started_{false};
    bool stopping_{false};
};

HttpServer::HttpServer(asio::io_context& io_context, const AppConfig& config,
                       StructuredLogger& logger, std::function<void()> on_runtime_failure)
    : impl_(std::make_unique<Impl>(io_context, config, logger, std::move(on_runtime_failure))) {}

HttpServer::~HttpServer() = default;

void HttpServer::start() { impl_->start(); }

void HttpServer::stop() noexcept { impl_->stop(); }

std::uint16_t HttpServer::bound_port() const noexcept { return impl_->bound_port(); }

}  // namespace apigate
