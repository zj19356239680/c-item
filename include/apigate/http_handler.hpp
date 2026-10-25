#ifndef APIGATE_HTTP_HANDLER_HPP_
#define APIGATE_HTTP_HANDLER_HPP_

#include <boost/beast/http.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "apigate/config.hpp"

namespace apigate
{

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

enum class GatewayFailure : std::uint8_t
{
    bad_gateway,
    gateway_timeout,
    gateway_overloaded,
    buffer_capacity_exhausted,
    no_healthy_upstream,
};

[[nodiscard]] HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request);
[[nodiscard]] bool is_metrics_target(const AppConfig& config, std::string_view target) noexcept;
[[nodiscard]] HttpResponse make_metrics_response(const HttpRequest& request, std::string body);
[[nodiscard]] std::string_view classify_http_route(std::string_view target) noexcept;
[[nodiscard]] bool should_proxy_http_request(const AppConfig& config,
                                             const HttpRequest& request) noexcept;
// Borrows immutable AppConfig storage; consumed synchronously by proxy construction.
struct SelectedUpstream
{
    const UpstreamConfig& upstream;
    std::string_view prefix;
    bool strip_prefix{false};
    std::optional<std::size_t> route_index{std::nullopt};
    bool strip_authorization{false};
    bool stream_response{false};
};

// Matches a route without selecting a backend or advancing any runtime state.
// Views remain valid while the immutable AppConfig remains alive.
struct SelectedRoute
{
    std::span<const UpstreamConfig> upstreams;
    std::string_view prefix;
    bool strip_prefix{false};
    std::optional<std::size_t> route_index{std::nullopt};
    const ApiKeyAuthConfig* auth{nullptr};
    bool stream_response{false};
};

[[nodiscard]] std::optional<SelectedRoute> select_http_route(const AppConfig& config,
                                                             const HttpRequest& request) noexcept;
[[nodiscard]] HttpResponse make_authentication_failed_response(const HttpRequest& request);
[[nodiscard]] HttpResponse make_gateway_error_response(const HttpRequest& request,
                                                       GatewayFailure failure);
[[nodiscard]] HttpResponse make_rate_limited_response(const HttpRequest& request,
                                                      std::uint32_t retry_after_seconds);

}  // namespace apigate

#endif  // APIGATE_HTTP_HANDLER_HPP_
