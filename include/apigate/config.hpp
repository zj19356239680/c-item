#ifndef APIGATE_CONFIG_HPP_
#define APIGATE_CONFIG_HPP_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apigate
{

enum class LogLevel : std::uint8_t
{
    trace,
    debug,
    info,
    warn,
    error,
    critical,
};

enum class UpstreamScheme : std::uint8_t
{
    http,
    https,
};

class UpstreamTlsContext;
class DownstreamTlsContext;

struct UpstreamConfig
{
    std::string host;
    std::uint16_t port;
    std::uint32_t timeout_ms{3000};
    UpstreamScheme scheme{UpstreamScheme::http};
};

struct RateLimitConfig
{
    std::uint32_t requests_per_second;
    std::uint32_t burst;
};

struct HealthCheckConfig
{
    std::string path;
    std::uint32_t interval_ms{5000};
    std::uint32_t timeout_ms{1000};
    std::uint32_t unhealthy_threshold{3};
    std::uint32_t healthy_threshold{2};
};

struct ApiKeyAuthConfig
{
    std::array<unsigned char, 32> digest{};
};

struct RouteConfig
{
    std::string prefix;
    std::vector<UpstreamConfig> upstreams;
    bool strip_prefix{false};
    std::optional<RateLimitConfig> rate_limit{std::nullopt};
    std::optional<HealthCheckConfig> health_check{std::nullopt};
    std::optional<ApiKeyAuthConfig> auth{std::nullopt};
    bool stream_response{false};
};

struct AppConfig
{
    std::string service_name{"api-gate"};
    std::string environment{"development"};
    LogLevel log_level{LogLevel::info};
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{8080};
    std::uint16_t max_connections{256};
    std::uint16_t max_concurrent_proxies{32};
    std::uint64_t max_in_flight_bytes{67108864};
    std::uint32_t shutdown_grace_ms{5000};
    bool metrics_enabled{false};
    bool upstream_keep_alive{false};
    std::optional<UpstreamConfig> upstream;
    std::vector<RouteConfig> routes;
    std::shared_ptr<UpstreamTlsContext> upstream_tls_context;
    std::shared_ptr<DownstreamTlsContext> downstream_tls_context;
};

class ConfigError : public std::runtime_error
{
   public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] AppConfig load_config_from_environment();
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

}  // namespace apigate

#endif  // APIGATE_CONFIG_HPP_
