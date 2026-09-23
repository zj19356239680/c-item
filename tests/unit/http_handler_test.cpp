#include "apigate/http_handler.hpp"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

namespace http = boost::beast::http;

[[nodiscard]] apigate::HttpRequest make_request(http::verb method, std::string_view target,
                                                bool keep_alive = true) {
    const boost::beast::string_view beast_target{target.data(), target.size()};
    apigate::HttpRequest request{method, beast_target, 11};
    request.set(http::field::host, "localhost");
    request.keep_alive(keep_alive);
    return request;
}

TEST(HttpHandlerTest, ReturnsHealthStatus) {
    const apigate::AppConfig config;
    const auto response =
        apigate::handle_http_request(config, make_request(http::verb::get, "/healthz"));

    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(response[http::field::content_type], "application/json");
    const auto body = nlohmann::json::parse(response.body());
    EXPECT_EQ(body.at("status"), "ok");
    EXPECT_EQ(body.at("service"), "api-gate");
}

TEST(HttpHandlerTest, ReturnsReadyStatusWithQueryString) {
    const apigate::AppConfig config;
    const auto response =
        apigate::handle_http_request(config, make_request(http::verb::get, "/readyz?probe=1"));

    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(nlohmann::json::parse(response.body()).at("status"), "ready");
}

TEST(HttpHandlerTest, ReturnsNotFoundForUnknownPath) {
    const apigate::AppConfig config;
    const auto response =
        apigate::handle_http_request(config, make_request(http::verb::get, "/missing"));

    EXPECT_EQ(response.result(), http::status::not_found);
    EXPECT_EQ(nlohmann::json::parse(response.body()).at("error").at("code"), "not_found");
}

TEST(HttpHandlerTest, RejectsUnsupportedMethod) {
    const apigate::AppConfig config;
    const auto response =
        apigate::handle_http_request(config, make_request(http::verb::post, "/healthz"));

    EXPECT_EQ(response.result(), http::status::method_not_allowed);
    EXPECT_EQ(response[http::field::allow], "GET");
}

TEST(HttpHandlerTest, PreservesKeepAlivePreference) {
    const apigate::AppConfig config;
    const auto persistent =
        apigate::handle_http_request(config, make_request(http::verb::get, "/healthz", true));
    const auto closing =
        apigate::handle_http_request(config, make_request(http::verb::get, "/healthz", false));

    EXPECT_TRUE(persistent.keep_alive());
    EXPECT_FALSE(closing.keep_alive());
}

}  // namespace
