#include "apigate/http_handler.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace apigate {
namespace {

namespace http = boost::beast::http;

[[nodiscard]] std::string_view request_path(std::string_view target) noexcept {
    const auto query_position = target.find('?');
    return target.substr(0, query_position);
}

[[nodiscard]] bool is_supported_proxy_target(std::string_view target) noexcept {
    return !target.empty() && target.front() == '/' && target.find('#') == std::string_view::npos;
}

[[nodiscard]] bool requests_upgrade(const HttpRequest& request) noexcept {
    return request.find(http::field::upgrade) != request.end();
}

[[nodiscard]] bool is_supported_proxy_method(http::verb method) noexcept {
    return method == http::verb::get || method == http::verb::post || method == http::verb::put ||
           method == http::verb::patch;
}

[[nodiscard]] HttpResponse make_json_response(const HttpRequest& request, http::status status,
                                              const nlohmann::json& body) {
    HttpResponse response{status, request.version()};
    response.set(http::field::server, "ApiGate");
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(request.keep_alive());
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

[[nodiscard]] HttpResponse make_method_not_allowed(const HttpRequest& request, bool proxy_enabled) {
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

std::string_view classify_http_route(std::string_view target) noexcept {
    const auto path = request_path(target);
    if (path == "/healthz") {
        return "healthz";
    }
    if (path == "/readyz") {
        return "readyz";
    }
    return "unmatched";
}

bool should_proxy_http_request(const AppConfig& config, const HttpRequest& request) noexcept {
    if (!config.upstream || !is_supported_proxy_method(request.method())) {
        return false;
    }
    const std::string_view target{request.target().data(), request.target().size()};
    if (classify_http_route(target) != "unmatched") {
        return false;
    }
    return is_supported_proxy_target(target) && !requests_upgrade(request) &&
           (request.method() != http::verb::get || request.body().empty());
}

HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request) {
    const std::string_view target{request.target().data(), request.target().size()};
    const auto route = classify_http_route(target);
    if (route != "unmatched" && request.method() != http::verb::get) {
        return make_method_not_allowed(request, false);
    }
    if (!is_supported_proxy_method(request.method())) {
        return make_method_not_allowed(request, config.upstream.has_value());
    }
    if (config.upstream && requests_upgrade(request)) {
        return make_json_response(
            request, http::status::bad_request,
            {{"error",
              {{"code", "unsupported_request"}, {"message", "request cannot be proxied"}}}});
    }
    if (route == "healthz") {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ok"}, {"service", config.service_name}});
    }
    if (route == "readyz") {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ready"}, {"service", config.service_name}});
    }
    if (!config.upstream && request.method() != http::verb::get) {
        return make_method_not_allowed(request, false);
    }
    if (config.upstream && (!is_supported_proxy_target(target) ||
                            (request.method() == http::verb::get && !request.body().empty()))) {
        return make_json_response(
            request, http::status::bad_request,
            {{"error",
              {{"code", "unsupported_request"}, {"message", "request cannot be proxied"}}}});
    }
    return make_json_response(request, http::status::not_found,
                              {{"error", {{"code", "not_found"}, {"message", "route not found"}}}});
}

HttpResponse make_gateway_error_response(const HttpRequest& request, GatewayFailure failure) {
    if (failure == GatewayFailure::gateway_timeout) {
        return make_json_response(
            request, http::status::gateway_timeout,
            {{"error", {{"code", "gateway_timeout"}, {"message", "upstream request timed out"}}}});
    }
    if (failure == GatewayFailure::gateway_overloaded) {
        return make_json_response(
            request, http::status::service_unavailable,
            {{"error",
              {{"code", "gateway_overloaded"}, {"message", "proxy capacity is exhausted"}}}});
    }
    return make_json_response(
        request, http::status::bad_gateway,
        {{"error", {{"code", "bad_gateway"}, {"message", "upstream request failed"}}}});
}

}  // namespace apigate
