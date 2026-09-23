#ifndef APIGATE_HTTP_HANDLER_HPP_
#define APIGATE_HTTP_HANDLER_HPP_

#include <boost/beast/http.hpp>
#include <string_view>

#include "apigate/config.hpp"

namespace apigate {

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

[[nodiscard]] HttpResponse handle_http_request(const AppConfig& config, const HttpRequest& request);
[[nodiscard]] std::string_view classify_http_route(std::string_view target) noexcept;

}  // namespace apigate

#endif  // APIGATE_HTTP_HANDLER_HPP_
