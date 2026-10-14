#include "apigate/application.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/system_error.hpp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

#include "apigate/config.hpp"
#include "apigate/http_server.hpp"
#include "apigate/logging.hpp"
#include "http_server_internal.hpp"

namespace apigate {
namespace {

void write_application_fallback(const char* event, int error_code) noexcept {
    if (std::fprintf(stderr, R"({"level":"error","payload":{"event":"%s","error_code":%d}}%c)",
                     event, error_code, 10) < 0) {
        std::clearerr(stderr);
    }
}

}  // namespace

class Application::Impl {
   public:
    Impl(const AppConfig& config, StructuredLogger& logger)
        : config_(config),
          logger_(logger),
          signals_(io_context_, SIGINT, SIGTERM),
          server_(
              io_context_, config_, logger_, [this]() noexcept { on_runtime_failure(); },
              [this]() noexcept { on_drain_complete(); }) {}

    [[nodiscard]] int run() {
        try {
            server_.start();
        } catch (const boost::system::system_error& error) {
            safe_critical("http_server_start_failed", error.code().value());
            return EXIT_FAILURE;
        }

        if (!arm_signal_wait()) {
            return exit_code_;
        }

        safe_info("service_started", [this] {
            return nlohmann::json{{"listen_address", config_.listen_address},
                                  {"listen_port", server_.bound_port()}};
        });
        const auto handlers_executed = io_context_.run();
        safe_info("service_stopped", [this, handlers_executed] {
            return nlohmann::json{{"exit_code", exit_code_},
                                  {"handlers_executed", handlers_executed}};
        });
        return exit_code_;
    }

   private:
    [[nodiscard]] bool arm_signal_wait() noexcept {
        try {
            signals_.async_wait(
                [this](const boost::system::error_code& error, int signal_number) noexcept {
                    on_signal(error, signal_number);
                });
            return true;
        } catch (const boost::system::system_error& error) {
            fail_signal_wait(error.code().value());
        } catch (...) {
            fail_signal_wait(0);
        }
        return false;
    }

    void on_signal(const boost::system::error_code& error, int signal_number) noexcept {
        switch (shutdown_signal_state_.on_wait_completion(error)) {
            case detail::ShutdownSignalDisposition::begin_drain:
                // Re-arm before doing any other work so a second signal can be queued
                // throughout logging and drain startup.
                if (!arm_signal_wait()) {
                    return;
                }
                safe_info("shutdown_signal_received",
                          [signal_number] { return nlohmann::json{{"signal", signal_number}}; });
                server_.begin_drain();
                return;
            case detail::ShutdownSignalDisposition::force_stop:
                server_.force_stop_drain(signal_number);
                return;
            case detail::ShutdownSignalDisposition::ignored:
                return;
            case detail::ShutdownSignalDisposition::fatal:
                fail_signal_wait(error.value());
                return;
        }
    }

    void fail_signal_wait(int error_code) noexcept {
        shutdown_signal_state_.mark_completed();
        safe_critical("signal_wait_failed", error_code);
        exit_code_ = EXIT_FAILURE;
        server_.stop();
        io_context_.stop();
    }

    void on_runtime_failure() noexcept {
        shutdown_signal_state_.mark_completed();
        exit_code_ = EXIT_FAILURE;
        io_context_.stop();
    }

    void on_drain_complete() noexcept {
        shutdown_signal_state_.mark_completed();
        try {
            signals_.cancel();
        } catch (const boost::system::system_error& error) {
            fail_signal_wait(error.code().value());
        } catch (...) {
            fail_signal_wait(0);
        }
        // HttpServer has released its final asynchronous work. The single io_context
        // exits naturally after the cancelled signal wait and other cancellation
        // completions have been dispatched.
    }

    template <typename FieldsFactory>
    void safe_info(std::string_view event, FieldsFactory&& make_fields) noexcept {
        try {
            logger_.info(event, std::forward<FieldsFactory>(make_fields)());
        } catch (...) {
            write_application_fallback("application_info_log_failed", 0);
        }
    }

    void safe_critical(std::string_view event, int error_code) noexcept {
        try {
            logger_.critical(event, {{"error_code", error_code}});
        } catch (...) {
            write_application_fallback("application_critical_log_failed", error_code);
        }
    }

    AppConfig config_;
    StructuredLogger& logger_;
    boost::asio::io_context io_context_;
    boost::asio::signal_set signals_;
    HttpServer server_;
    detail::ShutdownSignalState shutdown_signal_state_;
    int exit_code_{EXIT_SUCCESS};
};

Application::Application(const AppConfig& config, StructuredLogger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}

Application::~Application() = default;

int Application::run() { return impl_->run(); }

}  // namespace apigate
