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

HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request) {
    if (request.method() != http::verb::get) {
        auto response = make_json_response(
            request, http::status::method_not_allowed,
            {{"error", {{"code", "method_not_allowed"}, {"message", "only GET is supported"}}}});
        response.set(http::field::allow, "GET");
        return response;
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
    return make_json_response(request, http::status::not_found,
                              {{"error", {{"code", "not_found"}, {"message", "route not found"}}}});
}

}  // namespace apigate
