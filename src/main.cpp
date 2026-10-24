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
#include "apigate/version.hpp"

namespace
{

enum class Command : std::uint8_t
{
    run,
    check_config,
    help,
    version,
};

[[nodiscard]] Command parse_command_line(int argc, char* argv[])
{
    if (argc == 1)
    {
        return Command::run;
    }
    if (argc != 2)
    {
        throw std::invalid_argument("expected zero or one command-line option");
    }

    const std::string_view argument{argv[1]};
    if (argument == "--check-config")
    {
        return Command::check_config;
    }
    if (argument == "--help" || argument == "-h")
    {
        return Command::help;
    }
    if (argument == "--version")
    {
        return Command::version;
    }
    throw std::invalid_argument("unsupported command-line option");
}

void print_help()
{
    std::cout << "Usage: api-gate [--check-config | --help | --version]\n"
              << "\nEnvironment variables:\n"
              << "  APIGATE_SERVICE_NAME  Service label (default: api-gate)\n"
              << "  APIGATE_ENVIRONMENT   Runtime environment (default: development)\n"
              << "  APIGATE_LOG_LEVEL     trace|debug|info|warn|error|critical\n"
              << "  APIGATE_LISTEN_ADDRESS IP address to bind (default: 127.0.0.1)\n"
              << "  APIGATE_LISTEN_PORT   TCP port, 0 selects an ephemeral port (default: 8080)\n"
              << "  APIGATE_MAX_CONNECTIONS Active downstream connection limit (default: 256)\n"
              << "  APIGATE_MAX_CONCURRENT_PROXIES Concurrent upstream proxy limit (default: 32)\n"
              << "  APIGATE_MAX_IN_FLIGHT_BYTES Buffered body reservation budget in bytes "
                 "(default: 67108864)\n"
              << "  APIGATE_SHUTDOWN_GRACE_MS Drain deadline in milliseconds (default: 5000)\n"
              << "  APIGATE_METRICS_ENABLED true|false (default: false); expose GET /metrics to "
                 "listener clients\n"
              << "  APIGATE_TLS_CERT_FILE PEM server certificate chain; requires TLS key (default: "
                 "HTTP)\n"
              << "  APIGATE_TLS_KEY_FILE  Matching unencrypted PEM private key; enables "
                 "single-listener HTTPS\n"
              << "  APIGATE_UPSTREAM_KEEP_ALIVE true|false (default: false); reuse HTTP/HTTPS "
                 "connections\n"
              << "  APIGATE_UPSTREAM_HOST Optional static HTTP upstream host\n"
              << "  APIGATE_UPSTREAM_PORT Optional static HTTP upstream port (1-65535)\n"
              << "  APIGATE_UPSTREAM_TIMEOUT_MS Per-stage timeout in milliseconds (default: 3000)\n"
              << "  APIGATE_UPSTREAM_CA_FILE Optional PEM CA file replacing system HTTPS trust\n";
    std::cout
        << "  APIGATE_ROUTES        Optional JSON array of 1-32 static path routes (max 64 KiB);\n"
        << "                       prefix required; host/port with optional timeout_ms (default "
           "3000),\n"
        << "                       or upstreams with 1-16 host/port entries (forms exclusive);\n"
        << "                       each timeout_ms defaults to 3000; per-route round robin, no "
           "retry;\n"
        << "                       scheme is http (default) or https; port is always required;\n"
        << "                       strip_prefix is an optional boolean (default: false);\n"
        << "                       rate_limit optionally requires requests_per_second and burst\n"
        << "                       (integers 1-1000000); omitted means unlimited;\n"
        << "                       health_check is optional (disabled if omitted), requires path;\n"
        << "                       interval_ms=5000 (100-60000), timeout_ms=1000 (50-60000),\n"
        << "                       timeout <= interval; unhealthy_threshold=3, "
           "healthy_threshold=2\n"
        << "                       (integers 1-10); GET headers/status only, max 4 probes "
           "process-wide;\n"
        << "                       mutually exclusive with legacy upstream host/port/timeout "
           "variables\n";
}

void print_version()
{
    std::cout << "api-gate " << apigate::version << '\n';
}

void write_bootstrap_error(std::string_view category, std::string_view message) noexcept
{
    try
    {
        std::cerr << nlohmann::json{
                         {"level", "error"},
                         {"event", "bootstrap_failed"},
                         {"category", category},
                         {"message", message},
                     }
                         .dump()
                  << '\n';
    }
    catch (...)
    {
        std::cerr << "{\"level\":\"error\",\"event\":\"bootstrap_failed\"}\n";
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try
    {
        const Command command = parse_command_line(argc, argv);
        if (command == Command::help)
        {
            print_help();
            return EXIT_SUCCESS;
        }
        if (command == Command::version)
        {
            print_version();
            return EXIT_SUCCESS;
        }

        const apigate::AppConfig config = apigate::load_config_from_environment();
        apigate::StructuredLogger logger{config};
        if (command == Command::check_config)
        {
            nlohmann::json fields{
                {"log_level", apigate::to_string(config.log_level)},
                {"listen_address", config.listen_address},
                {"listen_port", config.listen_port},
                {"max_connections", config.max_connections},
                {"max_concurrent_proxies", config.max_concurrent_proxies},
                {"max_in_flight_bytes", config.max_in_flight_bytes},
                {"shutdown_grace_ms", config.shutdown_grace_ms},
                {"metrics_enabled", config.metrics_enabled},
                {"upstream_keep_alive", config.upstream_keep_alive},
                {"tls_enabled", config.downstream_tls_context != nullptr},
                {"proxy_enabled", config.upstream.has_value() || !config.routes.empty()}};
            if (!config.routes.empty())
            {
                fields["route_count"] = config.routes.size();
            }
            if (config.upstream)
            {
                fields["upstream_timeout_ms"] = config.upstream->timeout_ms;
            }
            logger.write_configuration_valid(fields);
            return EXIT_SUCCESS;
        }

        apigate::Application application{config, logger};
        return application.run();
    }
    catch (const apigate::ConfigError& error)
    {
        write_bootstrap_error("configuration", error.what());
    }
    catch (const std::invalid_argument& error)
    {
        write_bootstrap_error("arguments", error.what());
    }
    catch (const std::exception& error)
    {
        write_bootstrap_error("runtime", error.what());
    }
    catch (...)
    {
        write_bootstrap_error("runtime", "unknown failure");
    }
    return EXIT_FAILURE;
}
