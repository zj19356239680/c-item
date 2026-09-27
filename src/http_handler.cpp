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
    if (!config.upstream || request.method() != http::verb::get) {
        return false;
    }
    const std::string_view target{request.target().data(), request.target().size()};
    if (classify_http_route(target) != "unmatched") {
        return false;
    }
    return is_supported_proxy_target(target) && !requests_upgrade(request) &&
           request.body().empty();
}

HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request) {
    if (request.method() != http::verb::get) {
        auto response = make_json_response(
            request, http::status::method_not_allowed,
            {{"error", {{"code", "method_not_allowed"}, {"message", "only GET is supported"}}}});
        response.set(http::field::allow, "GET");
        return response;
    }

    if (config.upstream && requests_upgrade(request)) {
        return make_json_response(
            request, http::status::bad_request,
            {{"error",
              {{"code", "unsupported_request"}, {"message", "request cannot be proxied"}}}});
    }

    const std::string_view target{request.target().data(), request.target().size()};
    const auto route = classify_http_route(target);
    if (route == "healthz") {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ok"}, {"service", config.service_name}});
    }
    if (route == "readyz") {
        return make_json_response(request, http::status::ok,
                                  {{"status", "ready"}, {"service", config.service_name}});
    }
    if (config.upstream && (!is_supported_proxy_target(target) || !request.body().empty())) {
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
    return make_json_response(
        request, http::status::bad_gateway,
        {{"error", {{"code", "bad_gateway"}, {"message", "upstream request failed"}}}});
}

}  // namespace apigate
