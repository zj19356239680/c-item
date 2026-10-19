#include "apigate/http_handler.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace apigate
{
namespace
{

namespace http = boost::beast::http;

[[nodiscard]] std::string_view request_path(std::string_view target) noexcept
{
    const auto query_position = target.find('?');
    return target.substr(0, query_position);
}

[[nodiscard]] bool is_supported_proxy_target(std::string_view target) noexcept
{
    return !target.empty() && target.front() == '/' && target.find('#') == std::string_view::npos;
}

[[nodiscard]] bool requests_upgrade(const HttpRequest& request) noexcept
{
    return request.find(http::field::upgrade) != request.end();
}

[[nodiscard]] bool is_supported_proxy_method(http::verb method) noexcept
{
    return method == http::verb::get || method == http::verb::post || method == http::verb::put ||
           method == http::verb::patch;
}

[[nodiscard]] HttpResponse make_json_response(const HttpRequest& request, http::status status,
                                              const nlohmann::json& body)
{
    HttpResponse response{status, request.version()};
    response.set(http::field::server, "ApiGate");
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(request.keep_alive());
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

[[nodiscard]] HttpResponse make_method_not_allowed(const HttpRequest& request, bool proxy_enabled)
{
    const std::string_view allowed = proxy_enabled ? "GET, POST, PUT, PATCH" : "GET";
    const std::string_view message =
        proxy_enabled ? "method is not supported" : "only GET is supported";
    auto response =
        make_json_response(request, http::status::method_not_allowed,
                           {{"error", {{"code", "method_not_allowed"}, {"message", message}}}});
    response.set(http::field::allow, allowed);
    return response;
}

}  // namespace

std::string_view classify_http_route(std::string_view target) noexcept
{
    const auto path = request_path(target);
    if (path == "/healthz")
    {
        return "healthz";
    }
    if (path == "/readyz")
    {
        return "readyz";
    }
    return "unmatched";
}

bool should_proxy_http_request(const AppConfig& config, const HttpRequest& request) noexcept
{
    return select_http_route(config, request).has_value();
}

std::optional<SelectedRoute> select_http_route(const AppConfig& config,
                                               const HttpRequest& request) noexcept
{
    if (!is_supported_proxy_method(request.method()))
    {
        return std::nullopt;
    }
    const std::string_view target{request.target().data(), request.target().size()};
    if (classify_http_route(target) != "unmatched")
    {
        return std::nullopt;
    }
    if (!is_supported_proxy_target(target) || requests_upgrade(request) ||
        (request.method() == http::verb::get && !request.body().empty()))
    {
        return std::nullopt;
    }
    if (config.upstream)
    {
        return SelectedRoute{{&*config.upstream, 1}, {}, false};
    }
    const auto path = request_path(target);
    std::optional<SelectedRoute> selected;
    std::size_t longest = 0;
    for (std::size_t index = 0; index < config.routes.size(); ++index)
    {
        const auto& route = config.routes[index];
        const auto& prefix = route.prefix;
        if (prefix.size() > longest && path.starts_with(prefix) &&
            (prefix == "/" || path.size() == prefix.size() || path[prefix.size()] == '/'))
        {
            selected.emplace(
                SelectedRoute{route.upstreams, route.prefix, route.strip_prefix, index});
            longest = prefix.size();
        }
    }
    return selected;
}

HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request)
{
    const std::string_view target{request.target().data(), request.target().size()};
    const auto route = classify_http_route(target);
    const bool proxy_enabled = config.upstream.has_value() || !config.routes.empty();
    if (route != "unmatched" && request.method() != http::verb::get)
    {
        return make_method_not_allowed(request, false);
    }
    if (!is_supported_proxy_method(request.method()))
    {
        return make_method_not_allowed(request, proxy_enabled);
    }
    if (proxy_enabled && requests_upgrade(request))
    {
        return make_json_response(
            request, http::status::bad_request,
            {{"error",
              {{"code", "unsupported_request"}, {"message", "request cannot be proxied"}}}});
    }
    if (route == "healthz")
    {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ok"}, {"service", config.service_name}});
    }
    if (route == "readyz")
    {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ready"}, {"service", config.service_name}});
    }
    if (!proxy_enabled && request.method() != http::verb::get)
    {
        return make_method_not_allowed(request, false);
    }
    if (proxy_enabled && (!is_supported_proxy_target(target) ||
                          (request.method() == http::verb::get && !request.body().empty())))
    {
        return make_json_response(
            request, http::status::bad_request,
            {{"error",
              {{"code", "unsupported_request"}, {"message", "request cannot be proxied"}}}});
    }
    return make_json_response(request, http::status::not_found,
                              {{"error", {{"code", "not_found"}, {"message", "route not found"}}}});
}

HttpResponse make_rate_limited_response(const HttpRequest& request,
                                        std::uint32_t retry_after_seconds)
{
    auto response = make_json_response(
        request, http::status::too_many_requests,
        {{"error", {{"code", "rate_limited"}, {"message", "request rate limit is exceeded"}}}});
    response.set(http::field::retry_after, std::to_string(retry_after_seconds));
    return response;
}

HttpResponse make_gateway_error_response(const HttpRequest& request, GatewayFailure failure)
{
    if (failure == GatewayFailure::no_healthy_upstream)
    {
        return make_json_response(
            request, http::status::service_unavailable,
            {{"error",
              {{"code", "no_healthy_upstream"}, {"message", "no healthy upstream is available"}}}});
    }
    if (failure == GatewayFailure::gateway_timeout)
    {
        return make_json_response(
            request, http::status::gateway_timeout,
            {{"error", {{"code", "gateway_timeout"}, {"message", "upstream request timed out"}}}});
    }
    if (failure == GatewayFailure::gateway_overloaded)
    {
        return make_json_response(
            request, http::status::service_unavailable,
            {{"error",
              {{"code", "gateway_overloaded"}, {"message", "proxy capacity is exhausted"}}}});
    }
    if (failure == GatewayFailure::buffer_capacity_exhausted)
    {
        return make_json_response(
            request, http::status::service_unavailable,
            {{"error",
              {{"code", "gateway_overloaded"}, {"message", "buffer capacity is exhausted"}}}});
    }
    return make_json_response(
        request, http::status::bad_gateway,
        {{"error", {{"code", "bad_gateway"}, {"message", "upstream request failed"}}}});
}

}  // namespace apigate
