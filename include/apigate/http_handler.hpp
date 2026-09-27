#ifndef APIGATE_HTTP_HANDLER_HPP_
#define APIGATE_HTTP_HANDLER_HPP_

#include <boost/beast/http.hpp>
#include <cstdint>
#include <string_view>

#include "apigate/config.hpp"

namespace apigate {

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

enum class GatewayFailure : std::uint8_t {
    bad_gateway,
    gateway_timeout,
};

[[nodiscard]] HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request);
[[nodiscard]] std::string_view classify_http_route(std::string_view target) noexcept;
[[nodiscard]] bool should_proxy_http_request(const AppConfig& config,
                                             const HttpRequest& request) noexcept;
[[nodiscard]] HttpResponse make_gateway_error_response(const HttpRequest& request,
                                                       GatewayFailure failure);

}  // namespace apigate

#endif  // APIGATE_HTTP_HANDLER_HPP_
