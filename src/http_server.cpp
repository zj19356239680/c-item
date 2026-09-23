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
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "apigate/config.hpp"
#include "apigate/http_handler.hpp"
#include "apigate/logging.hpp"

namespace apigate {
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
                CloseCallback on_close)
        : stream_(std::move(socket)),
          config_(config),
          logger_(logger),
          on_close_(std::move(on_close)) {
        boost::system::error_code error;
        const auto endpoint = stream_.socket().remote_endpoint(error);
        remote_address_ = error ? "unknown" : endpoint.address().to_string();
    }

    void start() { read_request(); }

    void stop() noexcept {
        stopping_ = true;
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
    }

   private:
    void read_request() {
        if (stopping_) {
            finish();
            return;
        }

        parser_.emplace();
        parser_->header_limit(max_header_bytes);
        parser_->body_limit(max_body_bytes);
        stream_.expires_after(request_timeout);
        http::async_read(stream_, buffer_, *parser_,
                         [self = shared_from_this()](const boost::system::error_code& error,
                                                     std::size_t) { self->on_read(error); });
    }

    void on_read(const boost::system::error_code& error) {
        if (!error) {
            if (!parser_.has_value()) {
                logger_.error("http_parser_state_invalid");
                close_socket();
                return;
            }
            ++requests_served_;
            const auto& request = parser_.value().get();
            auto response = handle_http_request(config_, request);
            if (requests_served_ >= max_requests_per_connection) {
                response.keep_alive(false);
            }

            const std::string_view target{request.target().data(), request.target().size()};
            const std::string method{request.method_string().data(),
                                     request.method_string().size()};
            const std::string route{classify_http_route(target)};
            logger_.info("http_request_completed", {{"method", method},
                                                    {"route", route},
                                                    {"status", response.result_int()},
                                                    {"remote_address", remote_address_}});
            write_response(std::move(response));
            return;
        }

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
        response_.reset();
        if (error || close_after_write || stopping_) {
            close_socket();
            return;
        }
        read_request();
    }

    void close_socket() noexcept {
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
    CloseCallback on_close_;
    std::optional<http::request_parser<http::string_body>> parser_;
    std::optional<HttpResponse> response_;
    std::string remote_address_;
    std::size_t requests_served_{0};
    bool stopping_{false};
    bool finished_{false};
};

}  // namespace

class HttpServer::Impl {
   public:
    Impl(asio::io_context& io_context, const AppConfig& config, StructuredLogger& logger)
        : config_(config),
          logger_(logger),
          acceptor_(io_context),
          accept_retry_timer_(io_context) {}

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
        accept_next();
    }

    void stop() noexcept {
        if (stopping_) {
            return;
        }
        stopping_ = true;

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
    void accept_next() {
        acceptor_.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
            if (stopping_) {
                return;
            }
            if (error) {
                logger_.error("http_accept_failed", {{"error_code", error.value()}});
                accept_retry_timer_.expires_after(accept_retry_delay);
                accept_retry_timer_.async_wait(
                    [this](const boost::system::error_code& timer_error) {
                        if (!timer_error && !stopping_) {
                            accept_next();
                        }
                    });
                return;
            }

            auto session = std::make_shared<HttpSession>(
                std::move(socket), config_, logger_,
                [this](HttpSession* closed_session) { remove_session(closed_session); });
            sessions_.insert(session);
            session->start();
            accept_next();
        });
    }

    void remove_session(HttpSession* closed_session) noexcept {
        for (auto iterator = sessions_.begin(); iterator != sessions_.end(); ++iterator) {
            if (iterator->get() == closed_session) {
                sessions_.erase(iterator);
                return;
            }
        }
    }

    const AppConfig& config_;
    StructuredLogger& logger_;
    tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    std::set<std::shared_ptr<HttpSession>, std::owner_less<std::shared_ptr<HttpSession>>> sessions_;
    std::uint16_t bound_port_{0};
    bool started_{false};
    bool stopping_{false};
};

HttpServer::HttpServer(asio::io_context& io_context, const AppConfig& config,
                       StructuredLogger& logger)
    : impl_(std::make_unique<Impl>(io_context, config, logger)) {}

HttpServer::~HttpServer() = default;

void HttpServer::start() { impl_->start(); }

void HttpServer::stop() noexcept { impl_->stop(); }

std::uint16_t HttpServer::bound_port() const noexcept { return impl_->bound_port(); }

}  // namespace apigate
