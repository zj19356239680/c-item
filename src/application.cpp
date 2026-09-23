#include "apigate/application.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/system_error.hpp>
#include <csignal>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <utility>

#include "apigate/config.hpp"
#include "apigate/http_server.hpp"
#include "apigate/logging.hpp"

namespace apigate {

class Application::Impl {
   public:
    Impl(const AppConfig& config, StructuredLogger& logger)
        : config_(config), logger_(logger), signals_(io_context_, SIGINT, SIGTERM) {}

    [[nodiscard]] int run() {
        HttpServer server{io_context_, config_, logger_};
        try {
            server.start();
        } catch (const boost::system::system_error& error) {
            logger_.critical("http_server_start_failed", {{"error_code", error.code().value()}});
            return EXIT_FAILURE;
        }

        signals_.async_wait(
            [this, &server](const boost::system::error_code& error, int signal_number) {
                if (error) {
                    if (error != boost::asio::error::operation_aborted) {
                        logger_.critical("signal_wait_failed", {{"error_code", error.value()}});
                        exit_code_ = EXIT_FAILURE;
                        server.stop();
                    }
                    return;
                }

                logger_.info("shutdown_signal_received", {{"signal", signal_number}});
                server.stop();
            });

        logger_.info("service_started", {{"listen_address", config_.listen_address},
                                         {"listen_port", server.bound_port()}});
        const auto handlers_executed = io_context_.run();
        logger_.info("service_stopped",
                     {{"exit_code", exit_code_}, {"handlers_executed", handlers_executed}});
        return exit_code_;
    }

   private:
    AppConfig config_;
    StructuredLogger& logger_;
    boost::asio::io_context io_context_;
    boost::asio::signal_set signals_;
    int exit_code_{EXIT_SUCCESS};
};

Application::Application(const AppConfig& config, StructuredLogger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}

Application::~Application() = default;

int Application::run() { return impl_->run(); }

}  // namespace apigate
