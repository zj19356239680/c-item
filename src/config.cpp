#include "apigate/config.hpp"

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "upstream_tls.hpp"

namespace apigate
{
namespace
{

constexpr std::size_t max_label_length = 64;
constexpr std::size_t max_hostname_length = 253;
constexpr std::uint32_t max_timeout_ms = 60000;
constexpr std::uint64_t max_in_flight_bytes_limit = 1073741824;

[[nodiscard]] const char* raw_environment(const char* name)
{
    // Configuration is loaded once, before any worker threads can mutate the environment.
    return std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
}

[[nodiscard]] std::optional<std::string> read_environment(const char* name)
{
    const char* value = raw_environment(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    if (*value == '\0')
    {
        throw ConfigError(std::string{name} + " must not be empty");
    }
    return std::string{value};
}

void validate_label(std::string_view value, const char* variable_name)
{
    const bool valid_character =
        std::ranges::all_of(value,
                            [](unsigned char character)
                            {
                                return std::isalnum(character) != 0 || character == '-' ||
                                       character == '_' || character == '.';
                            });
    if (value.size() > max_label_length || !valid_character)
    {
        throw ConfigError(std::string{variable_name} +
                          " must be 1-64 characters using letters, digits, '.', '_' or '-'");
    }
}

[[nodiscard]] LogLevel parse_log_level(std::string_view value)
{
    if (value == "trace")
    {
        return LogLevel::trace;
    }
    if (value == "debug")
    {
        return LogLevel::debug;
    }
    if (value == "info")
    {
        return LogLevel::info;
    }
    if (value == "warn")
    {
        return LogLevel::warn;
    }
    if (value == "error")
    {
        return LogLevel::error;
    }
    if (value == "critical")
    {
        return LogLevel::critical;
    }
    throw ConfigError(
        "APIGATE_LOG_LEVEL must be one of trace, debug, info, warn, error or critical");
}

[[nodiscard]] std::string parse_listen_address(std::string_view value)
{
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(value, error);
    if (error)
    {
        throw ConfigError("APIGATE_LISTEN_ADDRESS must be a valid IPv4 or IPv6 address");
    }
    return address.to_string();
}

[[nodiscard]] std::uint16_t parse_listen_port(std::string_view value)
{
    unsigned int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        port > std::numeric_limits<std::uint16_t>::max())
    {
        throw ConfigError("APIGATE_LISTEN_PORT must be an integer from 0 to 65535");
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] std::uint16_t parse_upstream_port(std::string_view value)
{
    unsigned int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || port == 0 ||
        port > std::numeric_limits<std::uint16_t>::max())
    {
        throw ConfigError("APIGATE_UPSTREAM_PORT must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(port);
}

[[nodiscard]] std::uint16_t parse_capacity_limit(std::string_view value, const char* variable_name)
{
    unsigned int limit = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), limit);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || limit == 0 ||
        limit > std::numeric_limits<std::uint16_t>::max())
    {
        throw ConfigError(std::string{variable_name} + " must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(limit);
}

[[nodiscard]] std::uint32_t parse_bounded_milliseconds(std::string_view value,
                                                       const char* variable_name)
{
    std::uint32_t timeout = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), timeout);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || timeout == 0 ||
        timeout > max_timeout_ms)
    {
        throw ConfigError(std::string{variable_name} + " must be an integer from 1 to 60000");
    }
    return timeout;
}

[[nodiscard]] std::uint64_t parse_max_in_flight_bytes(std::string_view value)
{
    std::uint64_t bytes = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), bytes);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || bytes == 0 ||
        bytes > max_in_flight_bytes_limit)
    {
        throw ConfigError("APIGATE_MAX_IN_FLIGHT_BYTES must be an integer from 1 to 1073741824");
    }
    return bytes;
}

void validate_dns_hostname(std::string_view value)
{
    if (value.size() > max_hostname_length || value.front() == '.' || value.back() == '.')
    {
        throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
    }

    std::size_t label_length = 0;
    bool label_starts_with_hyphen = false;
    char previous = '\0';
    for (const char raw_character : value)
    {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '.')
        {
            if (label_length == 0 || label_length > 63 || label_starts_with_hyphen ||
                previous == '-')
            {
                throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
            }
            label_length = 0;
            label_starts_with_hyphen = false;
        }
        else if (std::isalnum(character) != 0 || character == '-')
        {
            if (label_length == 0)
            {
                label_starts_with_hyphen = character == '-';
            }
            ++label_length;
        }
        else
        {
            throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
        }
        previous = static_cast<char>(character);
    }
    if (label_length == 0 || label_length > 63 || label_starts_with_hyphen || previous == '-')
    {
        throw ConfigError("APIGATE_UPSTREAM_HOST must be a valid hostname or IP address");
    }
}

[[nodiscard]] std::string parse_upstream_host(std::string_view value)
{
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(value, error);
    if (!error)
    {
        return address.to_string();
    }
    validate_dns_hostname(value);
    return std::string{value};
}

void validate_route_prefix(std::string_view prefix)
{
    if (prefix.empty() || prefix.front() != '/')
    {
        throw ConfigError("APIGATE_ROUTES prefix must start with '/'");
    }
    if (prefix == "/")
    {
        return;
    }
    std::size_t begin = 1;
    while (begin <= prefix.size())
    {
        const auto end = prefix.find('/', begin);
        const auto segment =
            prefix.substr(begin, end == std::string_view::npos ? end : end - begin);
        const bool valid = std::ranges::all_of(segment,
                                               [](char character)
                                               {
                                                   return (character >= 'a' && character <= 'z') ||
                                                          (character >= 'A' && character <= 'Z') ||
                                                          (character >= '0' && character <= '9') ||
                                                          character == '-' || character == '.' ||
                                                          character == '_' || character == '~';
                                               });
        if (segment.empty() || segment == "." || segment == ".." || !valid)
        {
            throw ConfigError("APIGATE_ROUTES prefix contains an invalid path segment");
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1;
    }
}

[[nodiscard]] std::uint64_t route_integer(const nlohmann::json& value, std::uint64_t maximum)
{
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    {
        throw ConfigError("APIGATE_ROUTES integer field must be a positive integer");
    }
    const auto number = value.get<std::uint64_t>();
    if (number == 0 || number > maximum)
    {
        throw ConfigError("APIGATE_ROUTES integer field is out of range");
    }
    return number;
}

[[nodiscard]] UpstreamConfig parse_route_upstream(const nlohmann::json& entry)
{
    if (!entry.is_object() || !entry.contains("host") || !entry.contains("port") ||
        !entry.at("host").is_string())
    {
        throw ConfigError("APIGATE_ROUTES upstream requires string host and integer port");
    }
    const auto host = entry.at("host").get<std::string>();
    std::string validated_host;
    try
    {
        if (host.empty())
        {
            throw ConfigError("empty host");
        }
        validated_host = parse_upstream_host(host);
    }
    catch (const ConfigError&)
    {
        throw ConfigError("APIGATE_ROUTES host must be a valid hostname or IP address");
    }
    const auto port = static_cast<std::uint16_t>(route_integer(entry.at("port"), 65535));
    const auto timeout =
        entry.contains("timeout_ms")
            ? static_cast<std::uint32_t>(route_integer(entry.at("timeout_ms"), 60000))
            : 3000U;
    UpstreamScheme scheme = UpstreamScheme::http;
    if (entry.contains("scheme"))
    {
        const auto& value = entry.at("scheme");
        if (!value.is_string() || (value != "http" && value != "https"))
        {
            throw ConfigError("APIGATE_ROUTES scheme must be http or https");
        }
        scheme = value == "https" ? UpstreamScheme::https : UpstreamScheme::http;
    }
    return {std::move(validated_host), port, timeout, scheme};
}

[[nodiscard]] std::vector<RouteConfig> parse_routes(std::string_view value)
{
    if (value.size() > std::size_t{64} * 1024)
    {
        throw ConfigError("APIGATE_ROUTES must not exceed 65536 bytes");
    }
    using Json = nlohmann::json;
    std::array<std::set<std::string>, 3> keys;
    std::string route_key;
    std::size_t object_count = 0;
    std::size_t upstream_count = 0;
    bool in_upstreams = false;
    const auto callback = [&keys, &route_key, &object_count, &upstream_count, &in_upstreams](
                              int depth, Json::parse_event_t event, Json& parsed)
    {
        if (event == Json::parse_event_t::array_start && depth != 0)
        {
            if (depth != 2 || route_key != "upstreams")
            {
                throw ConfigError("APIGATE_ROUTES contains an invalid nested array");
            }
            upstream_count = 0;
            in_upstreams = true;
        }
        if (event == Json::parse_event_t::array_end && depth == 2)
        {
            in_upstreams = false;
        }
        if (event == Json::parse_event_t::object_start)
        {
            if (depth == 1)
            {
                if (++object_count > 32)
                {
                    throw ConfigError("APIGATE_ROUTES must contain at most 32 route objects");
                }
                keys[0].clear();
            }
            else if (depth == 2 && route_key == "rate_limit")
            {
                keys[1].clear();
            }
            else if (depth == 3 && in_upstreams && route_key == "upstreams")
            {
                if (++upstream_count > 16)
                {
                    throw ConfigError("APIGATE_ROUTES upstreams must contain at most 16 entries");
                }
                keys[2].clear();
            }
            else
            {
                throw ConfigError("APIGATE_ROUTES contains an invalid nested object");
            }
        }
        if (event == Json::parse_event_t::key)
        {
            if (depth < 2 || depth > 4 ||
                !keys.at(static_cast<std::size_t>(depth - 2))
                     .insert(parsed.get<std::string>())
                     .second)
            {
                throw ConfigError("APIGATE_ROUTES contains a duplicate object field");
            }
            if (depth == 2)
            {
                route_key = parsed.get<std::string>();
            }
        }
        return true;
    };
    Json document;
    try
    {
        document = Json::parse(value, callback);
    }
    catch (const Json::exception&)
    {
        throw ConfigError("APIGATE_ROUTES must contain valid JSON");
    }
    if (!document.is_array() || document.empty() || document.size() > 32)
    {
        throw ConfigError("APIGATE_ROUTES must contain 1-32 route objects");
    }
    std::vector<RouteConfig> routes;
    std::set<std::string> prefixes;
    for (const auto& entry : document)
    {
        if (!entry.is_object() || !entry.contains("prefix"))
        {
            throw ConfigError("APIGATE_ROUTES entry requires prefix and an upstream configuration");
        }
        for (const auto& item : entry.items())
        {
            if (item.key() != "prefix" && item.key() != "host" && item.key() != "port" &&
                item.key() != "timeout_ms" && item.key() != "strip_prefix" &&
                item.key() != "rate_limit" && item.key() != "upstreams" && item.key() != "scheme")
            {
                throw ConfigError("APIGATE_ROUTES entry contains an unknown field");
            }
        }
        if (!entry.at("prefix").is_string())
        {
            throw ConfigError("APIGATE_ROUTES prefix must be a string");
        }
        auto prefix = entry.at("prefix").get<std::string>();
        validate_route_prefix(prefix);
        if (!prefixes.insert(prefix).second)
        {
            throw ConfigError("APIGATE_ROUTES contains a duplicate prefix");
        }
        std::vector<UpstreamConfig> upstreams;
        if (entry.contains("upstreams"))
        {
            if (entry.contains("host") || entry.contains("port") || entry.contains("timeout_ms") ||
                entry.contains("scheme"))
            {
                throw ConfigError(
                    "APIGATE_ROUTES upstreams conflicts with route-level upstream fields");
            }
            const auto& pool = entry.at("upstreams");
            if (!pool.is_array() || pool.empty() || pool.size() > 16)
            {
                throw ConfigError("APIGATE_ROUTES upstreams must contain 1-16 upstream objects");
            }
            for (const auto& backend : pool)
            {
                if (!backend.is_object())
                {
                    throw ConfigError("APIGATE_ROUTES upstream entry must be an object");
                }
                for (const auto& item : backend.items())
                {
                    if (item.key() != "host" && item.key() != "port" &&
                        item.key() != "timeout_ms" && item.key() != "scheme")
                    {
                        throw ConfigError(
                            "APIGATE_ROUTES upstream entry contains an unknown field");
                    }
                }
                upstreams.push_back(parse_route_upstream(backend));
            }
        }
        else
        {
            upstreams.push_back(parse_route_upstream(entry));
        }
        if (entry.contains("strip_prefix") && !entry.at("strip_prefix").is_boolean())
        {
            throw ConfigError("APIGATE_ROUTES strip_prefix must be a boolean");
        }
        const bool strip_prefix = entry.value("strip_prefix", false);
        std::optional<RateLimitConfig> rate_limit;
        if (entry.contains("rate_limit"))
        {
            const auto& limit = entry.at("rate_limit");
            if (!limit.is_object() || limit.size() != 2 || !limit.contains("requests_per_second") ||
                !limit.contains("burst"))
            {
                throw ConfigError(
                    "APIGATE_ROUTES rate_limit requires only requests_per_second and burst");
            }
            rate_limit = RateLimitConfig{
                static_cast<std::uint32_t>(route_integer(limit.at("requests_per_second"), 1000000)),
                static_cast<std::uint32_t>(route_integer(limit.at("burst"), 1000000))};
        }
        routes.push_back({std::move(prefix), std::move(upstreams), strip_prefix, rate_limit});
    }
    return routes;
}

}  // namespace

AppConfig load_config_from_environment()
{
    AppConfig config;
    if (const auto value = read_environment("APIGATE_SERVICE_NAME"))
    {
        validate_label(*value, "APIGATE_SERVICE_NAME");
        config.service_name = *value;
    }
    if (const auto value = read_environment("APIGATE_ENVIRONMENT"))
    {
        validate_label(*value, "APIGATE_ENVIRONMENT");
        config.environment = *value;
    }
    if (const auto value = read_environment("APIGATE_LOG_LEVEL"))
    {
        config.log_level = parse_log_level(*value);
    }
    if (const auto value = read_environment("APIGATE_LISTEN_ADDRESS"))
    {
        config.listen_address = parse_listen_address(*value);
    }
    if (const auto value = read_environment("APIGATE_LISTEN_PORT"))
    {
        config.listen_port = parse_listen_port(*value);
    }
    if (const auto value = read_environment("APIGATE_MAX_CONNECTIONS"))
    {
        config.max_connections = parse_capacity_limit(*value, "APIGATE_MAX_CONNECTIONS");
    }
    if (const auto value = read_environment("APIGATE_MAX_CONCURRENT_PROXIES"))
    {
        config.max_concurrent_proxies =
            parse_capacity_limit(*value, "APIGATE_MAX_CONCURRENT_PROXIES");
    }
    if (const auto value = read_environment("APIGATE_MAX_IN_FLIGHT_BYTES"))
    {
        config.max_in_flight_bytes = parse_max_in_flight_bytes(*value);
    }
    if (const auto value = read_environment("APIGATE_SHUTDOWN_GRACE_MS"))
    {
        config.shutdown_grace_ms = parse_bounded_milliseconds(*value, "APIGATE_SHUTDOWN_GRACE_MS");
    }

    if (raw_environment("APIGATE_ROUTES") != nullptr &&
        (raw_environment("APIGATE_UPSTREAM_HOST") != nullptr ||
         raw_environment("APIGATE_UPSTREAM_PORT") != nullptr ||
         raw_environment("APIGATE_UPSTREAM_TIMEOUT_MS") != nullptr))
    {
        throw ConfigError("APIGATE_ROUTES conflicts with legacy upstream variables");
    }
    if (const auto routes = read_environment("APIGATE_ROUTES"))
    {
        config.routes = parse_routes(*routes);
        const bool https_enabled = std::ranges::any_of(
            config.routes,
            [](const RouteConfig& route)
            {
                return std::ranges::any_of(route.upstreams,
                                           [](const UpstreamConfig& upstream)
                                           {
                                               return upstream.scheme == UpstreamScheme::https;
                                           });
            });
        config.upstream_tls_context =
            make_upstream_tls_context(read_environment("APIGATE_UPSTREAM_CA_FILE"), https_enabled);
        return config;
    }
    const auto upstream_host = read_environment("APIGATE_UPSTREAM_HOST");
    const auto upstream_port = read_environment("APIGATE_UPSTREAM_PORT");
    const auto upstream_timeout = read_environment("APIGATE_UPSTREAM_TIMEOUT_MS");
    if (upstream_host.has_value() != upstream_port.has_value() ||
        (upstream_timeout.has_value() && !upstream_host.has_value()))
    {
        throw ConfigError(
            "APIGATE_UPSTREAM_HOST and APIGATE_UPSTREAM_PORT must be set together; "
            "APIGATE_UPSTREAM_TIMEOUT_MS requires both");
    }
    if (upstream_host)
    {
        UpstreamConfig upstream{parse_upstream_host(*upstream_host),
                                parse_upstream_port(*upstream_port), 3000};
        if (upstream_timeout)
        {
            upstream.timeout_ms =
                parse_bounded_milliseconds(*upstream_timeout, "APIGATE_UPSTREAM_TIMEOUT_MS");
        }
        config.upstream = std::move(upstream);
    }
    config.upstream_tls_context =
        make_upstream_tls_context(read_environment("APIGATE_UPSTREAM_CA_FILE"), false);
    return config;
}

std::string_view to_string(LogLevel level) noexcept
{
    switch (level)
    {
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
