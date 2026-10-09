#include "apigate/config.hpp"

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace apigate {
namespace {

constexpr std::size_t max_label_length = 64;
constexpr std::size_t max_hostname_length = 253;
constexpr std::uint32_t max_timeout_ms = 60000;

[[nodiscard]] std::optional<std::string> read_environment(const char* name) {
    // Configuration is loaded once, before any worker threads can mutate the environment.
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr) {
        return std::nullopt;
    }
    if (*value == '\0') {
        throw ConfigError(std::string{name} + " must not be empty");
    }
    return std::string{value};
}

void validate_label(std::string_view value, const char* variable_name) {
    const bool valid_character = std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.';
    });
    if (value.size() > max_label_length || !valid_character) {
        throw ConfigError(std::string{variable_name} +
                          " must be 1-64 characters using letters, digits, '.', '_' or '-'");
    }
}

[[nodiscard]] LogLevel parse_log_level(std::string_view value) {
    if (value == "trace") {
        return LogLevel::trace;
    }
    if (value == "debug") {
        return LogLevel::debug;
    }
    if (value == "info") {
        return LogLevel::info;
    }
    if (value == "warn") {
        return LogLevel::warn;
    }
    if (value == "error") {
        return LogLevel::error;
    }
    if (value == "critical") {
        return LogLevel::critical;
    }
    throw ConfigError(
        "APIGATE_LOG_LEVEL must be one of trace, debug, info, warn, error or critical");
}

[[nodiscard]] std::string parse_listen_address(std::string_view value) {
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(value, error);
    if (error) {
        throw ConfigError("APIGATE_LISTEN_ADDRESS must be a valid IPv4 or IPv6 address");
    }
    return address.to_string();
}

[[nodiscard]] std::uint16_t parse_listen_port(std::string_view value) {
    unsigned int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        port > std::numeric_limits<std::uint16_t>::max()) {
        throw ConfigError("APIGATE_LISTEN_PORT must be an integer from 0 to 65535");
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] std::uint16_t parse_upstream_port(std::string_view value) {
    unsigned int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || port == 0 ||
        port > std::numeric_limits<std::uint16_t>::max()) {
        throw ConfigError("APIGATE_UPSTREAM_PORT must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] std::uint16_t parse_capacity_limit(std::string_view value,
                                                 const char* variable_name) {
    unsigned int limit = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), limit);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || limit == 0 ||
        limit > std::numeric_limits<std::uint16_t>::max()) {
        throw ConfigError(std::string{variable_name} + " must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(limit);
}

[[nodiscard]] std::uint32_t parse_bounded_milliseconds(std::string_view value,
                                                       const char* variable_name) {
    std::uint32_t timeout = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), timeout);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || timeout == 0 ||
        timeout > max_timeout_ms) {
        throw ConfigError(std::string{variable_name} + " must be an integer from 1 to 60000");
    }
    return timeout;
}

void validate_dns_hostname(std::string_view value) {
    if (value.size() > max_hostname_length || value.front() == '.' || value.back() == '.') {
        throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
    }

    std::size_t label_length = 0;
    bool label_starts_with_hyphen = false;
    char previous = '\0';
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '.') {
            if (label_length == 0 || label_length > 63 || label_starts_with_hyphen ||
                previous == '-') {
                throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
            }
            label_length = 0;
            label_starts_with_hyphen = false;
        } else if (std::isalnum(character) != 0 || character == '-') {
            if (label_length == 0) {
                label_starts_with_hyphen = character == '-';
            }
            ++label_length;
        } else {
            throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
        }
        previous = static_cast<char>(character);
    }
    if (label_length == 0 || label_length > 63 || label_starts_with_hyphen || previous == '-') {
        throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
    }
}

[[nodiscard]] std::string parse_upstream_host(std::string_view value) {
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(value, error);
    if (!error) {
        return address.to_string();
    }
    validate_dns_hostname(value);
    return std::string{value};
}

}  // namespace

AppConfig load_config_from_environment() {
    AppConfig config;
    if (const auto value = read_environment("APIGATE_SERVICE_NAME")) {
        validate_label(*value, "APIGATE_SERVICE_NAME");
        config.service_name = *value;
    }
    if (const auto value = read_environment("APIGATE_ENVIRONMENT")) {
        validate_label(*value, "APIGATE_ENVIRONMENT");
        config.environment = *value;
    }
    if (const auto value = read_environment("APIGATE_LOG_LEVEL")) {
        config.log_level = parse_log_level(*value);
    }
    if (const auto value = read_environment("APIGATE_LISTEN_ADDRESS")) {
        config.listen_address = parse_listen_address(*value);
    }
    if (const auto value = read_environment("APIGATE_LISTEN_PORT")) {
        config.listen_port = parse_listen_port(*value);
    }
    if (const auto value = read_environment("APIGATE_MAX_CONNECTIONS")) {
        config.max_connections = parse_capacity_limit(*value, "APIGATE_MAX_CONNECTIONS");
    }
    if (const auto value = read_environment("APIGATE_MAX_CONCURRENT_PROXIES")) {
        config.max_concurrent_proxies =
            parse_capacity_limit(*value, "APIGATE_MAX_CONCURRENT_PROXIES");
    }
    if (const auto value = read_environment("APIGATE_SHUTDOWN_GRACE_MS")) {
        config.shutdown_grace_ms = parse_bounded_milliseconds(*value, "APIGATE_SHUTDOWN_GRACE_MS");
    }

    const auto upstream_host = read_environment("APIGATE_UPSTREAM_HOST");
    const auto upstream_port = read_environment("APIGATE_UPSTREAM_PORT");
    const auto upstream_timeout = read_environment("APIGATE_UPSTREAM_TIMEOUT_MS");
    if (upstream_host.has_value() != upstream_port.has_value() ||
        (upstream_timeout.has_value() && !upstream_host.has_value())) {
        throw ConfigError(
            "APIGATE_UPSTREAM_HOST and APIGATE_UPSTREAM_PORT must be set together; "
            "APIGATE_UPSTREAM_TIMEOUT_MS requires both");
    }
    if (upstream_host) {
        UpstreamConfig upstream{parse_upstream_host(*upstream_host),
                                parse_upstream_port(*upstream_port), 3000};
        if (upstream_timeout) {
            upstream.timeout_ms =
                parse_bounded_milliseconds(*upstream_timeout, "APIGATE_UPSTREAM_TIMEOUT_MS");
        }
        config.upstream = std::move(upstream);
    }
    return config;
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace:
            return "trace";
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warn:
            return "warn";
        case LogLevel::error:
            return "error";
        case LogLevel::critical:
            return "critical";
    }
    return "unknown";
}

}  // namespace apigate
