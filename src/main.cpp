#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

#include "apigate/application.hpp"
#include "apigate/config.hpp"
#include "apigate/logging.hpp"

namespace {

enum class Command : std::uint8_t {
    run,
    check_config,
    help,
};

[[nodiscard]] Command parse_command_line(int argc, char* argv[]) {
    if (argc == 1) {
        return Command::run;
    }
    if (argc != 2) {
        throw std::invalid_argument("expected zero or one command-line option");
    }

    const std::string_view argument{argv[1]};
    if (argument == "--check-config") {
        return Command::check_config;
    }
    if (argument == "--help" || argument == "-h") {
        return Command::help;
    }
    throw std::invalid_argument("unsupported command-line option");
}

void print_help() {
    std::cout << "Usage: api-gate [--check-config | --help]\n"
              << "\nEnvironment variables:\n"
              << "  APIGATE_SERVICE_NAME  Service label (default: api-gate)\n"
              << "  APIGATE_ENVIRONMENT   Runtime environment (default: development)\n"
              << "  APIGATE_LOG_LEVEL     trace|debug|info|warn|error|critical\n"
              << "  APIGATE_LISTEN_ADDRESS IP address to bind (default: 127.0.0.1)\n"
              << "  APIGATE_LISTEN_PORT   TCP port, 0 selects an ephemeral port (default: 8080)\n";
}

void write_bootstrap_error(std::string_view category, std::string_view message) noexcept {
    try {
        std::cerr << nlohmann::json{
                         {"level", "error"},
                         {"event", "bootstrap_failed"},
                         {"category", category},
                         {"message", message},
                     }
                         .dump()
                  << '\n';
    } catch (...) {
        std::cerr << "{\"level\":\"error\",\"event\":\"bootstrap_failed\"}\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Command command = parse_command_line(argc, argv);
        if (command == Command::help) {
            print_help();
            return EXIT_SUCCESS;
        }

        const apigate::AppConfig config = apigate::load_config_from_environment();
        apigate::StructuredLogger logger{config};
        if (command == Command::check_config) {
            logger.write_configuration_valid({{"log_level", apigate::to_string(config.log_level)},
                                              {"listen_address", config.listen_address},
                                              {"listen_port", config.listen_port}});
            return EXIT_SUCCESS;
        }

        apigate::Application application{config, logger};
        return application.run();
    } catch (const apigate::ConfigError& error) {
        write_bootstrap_error("configuration", error.what());
    } catch (const std::invalid_argument& error) {
        write_bootstrap_error("arguments", error.what());
    } catch (const std::exception& error) {
        write_bootstrap_error("runtime", error.what());
    } catch (...) {
        write_bootstrap_error("runtime", "unknown failure");
    }
    return EXIT_FAILURE;
}
