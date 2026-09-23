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

namespace apigate {
namespace {

constexpr std::size_t max_label_length = 64;

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
