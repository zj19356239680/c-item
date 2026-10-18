#include "apigate/http_handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <string>
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

[[nodiscard]] apigate::AppConfig proxy_config() {
    apigate::AppConfig config;
    config.upstream = apigate::UpstreamConfig{"upstream.internal", 8080, 3000};
    return config;
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

TEST(HttpHandlerTest, KeepsHealthRoutesLocalWhenProxyIsEnabled) {
    const auto config = proxy_config();

    EXPECT_FALSE(apigate::should_proxy_http_request(
        config, make_request(http::verb::get, "/healthz?probe=1")));
    EXPECT_FALSE(
        apigate::should_proxy_http_request(config, make_request(http::verb::get, "/readyz")));
    EXPECT_EQ(
        apigate::handle_http_request(config, make_request(http::verb::get, "/healthz")).result(),
        http::status::ok);
}

TEST(HttpHandlerTest, ProxiesSupportedMethodsOnUnmatchedRoutes) {
    const auto config = proxy_config();
    auto proxy_request = make_request(http::verb::get, "/resource?key=value");

    EXPECT_TRUE(apigate::should_proxy_http_request(config, proxy_request));
    for (const auto method : {http::verb::post, http::verb::put, http::verb::patch}) {
        auto body_request = make_request(method, "/resource?key=value");
        EXPECT_TRUE(apigate::should_proxy_http_request(config, body_request));
        body_request.body() = "bounded-body";
        body_request.prepare_payload();
        EXPECT_TRUE(apigate::should_proxy_http_request(config, body_request));
    }
    EXPECT_FALSE(apigate::should_proxy_http_request(apigate::AppConfig{},
                                                    make_request(http::verb::get, "/resource")));
    EXPECT_FALSE(
        apigate::should_proxy_http_request(config, make_request(http::verb::delete_, "/resource")));

    proxy_request.body() = "not-forwarded";
    proxy_request.prepare_payload();
    EXPECT_FALSE(apigate::should_proxy_http_request(config, proxy_request));
    const auto body_error = apigate::handle_http_request(config, proxy_request);
    EXPECT_EQ(body_error.result(), http::status::bad_request);
    EXPECT_EQ(nlohmann::json::parse(body_error.body()).at("error").at("code"),
              "unsupported_request");
}

TEST(HttpHandlerTest, KeepsBodyMethodsDisabledWithoutAnUpstream) {
    const apigate::AppConfig config;
    for (const auto method : {http::verb::post, http::verb::put, http::verb::patch}) {
        const auto response =
            apigate::handle_http_request(config, make_request(method, "/resource"));
        EXPECT_EQ(response.result(), http::status::method_not_allowed);
        EXPECT_EQ(response[http::field::allow], "GET");
    }
}

TEST(HttpHandlerTest, KeepsLocalRoutesGetOnlyWhenProxyIsEnabled) {
    const auto config = proxy_config();
    for (const auto method : {http::verb::post, http::verb::put, http::verb::patch}) {
        const auto request = make_request(method, "/healthz");
        EXPECT_FALSE(apigate::should_proxy_http_request(config, request));
        const auto response = apigate::handle_http_request(config, request);
        EXPECT_EQ(response.result(), http::status::method_not_allowed);
        EXPECT_EQ(response[http::field::allow], "GET");
    }
}

TEST(HttpHandlerTest, RejectsMethodsOutsideTheProxyMvp) {
    const auto config = proxy_config();
    for (const auto method :
         {http::verb::head, http::verb::delete_, http::verb::options, http::verb::connect}) {
        const auto request = make_request(method, "/resource");
        EXPECT_FALSE(apigate::should_proxy_http_request(config, request));
        const auto response = apigate::handle_http_request(config, request);
        EXPECT_EQ(response.result(), http::status::method_not_allowed);
        EXPECT_EQ(response[http::field::allow], "GET, POST, PUT, PATCH");
    }
}

TEST(HttpHandlerTest, RejectsUnsupportedProxyTargetAndUpgrade) {
    const auto config = proxy_config();
    const auto absolute = make_request(http::verb::get, "http://example.test/resource");
    auto upgrade = make_request(http::verb::get, "/resource");
    upgrade.set(http::field::upgrade, "websocket");

    EXPECT_FALSE(apigate::should_proxy_http_request(config, absolute));
    EXPECT_EQ(apigate::handle_http_request(config, absolute).result(), http::status::bad_request);
    EXPECT_FALSE(apigate::should_proxy_http_request(config, upgrade));
    EXPECT_EQ(apigate::handle_http_request(config, upgrade).result(), http::status::bad_request);
    upgrade.target("/healthz");
    EXPECT_EQ(apigate::handle_http_request(config, upgrade).result(), http::status::bad_request);
}

TEST(HttpHandlerTest, CreatesSafeGatewayErrors) {
    const auto request = make_request(http::verb::get, "/private?secret=value");
    const auto bad_gateway =
        apigate::make_gateway_error_response(request, apigate::GatewayFailure::bad_gateway);
    const auto timeout =
        apigate::make_gateway_error_response(request, apigate::GatewayFailure::gateway_timeout);
    const auto overloaded =
        apigate::make_gateway_error_response(request, apigate::GatewayFailure::gateway_overloaded);
    const auto buffer_exhausted = apigate::make_gateway_error_response(
        request, apigate::GatewayFailure::buffer_capacity_exhausted);

    EXPECT_EQ(bad_gateway.result(), http::status::bad_gateway);
    EXPECT_EQ(nlohmann::json::parse(bad_gateway.body()).at("error").at("code"), "bad_gateway");
    EXPECT_EQ(bad_gateway.body().find("private"), std::string::npos);
    EXPECT_EQ(bad_gateway.body().find("secret"), std::string::npos);
    EXPECT_EQ(timeout.result(), http::status::gateway_timeout);
    EXPECT_EQ(nlohmann::json::parse(timeout.body()).at("error").at("code"), "gateway_timeout");
    EXPECT_EQ(overloaded.result(), http::status::service_unavailable);
    EXPECT_EQ(nlohmann::json::parse(overloaded.body()).at("error").at("code"),
              "gateway_overloaded");
    EXPECT_EQ(nlohmann::json::parse(overloaded.body()).at("error").at("message"),
              "proxy capacity is exhausted");
    EXPECT_EQ(overloaded[http::field::content_type], "application/json");
    EXPECT_EQ(overloaded[http::field::cache_control], "no-store");
    EXPECT_EQ(overloaded[http::field::server], "ApiGate");
    EXPECT_TRUE(overloaded.keep_alive());
    EXPECT_EQ(overloaded.find(http::field::retry_after), overloaded.end());
    EXPECT_EQ(buffer_exhausted.result(), http::status::service_unavailable);
    EXPECT_EQ(nlohmann::json::parse(buffer_exhausted.body()).at("error").at("code"),
              "gateway_overloaded");
    EXPECT_EQ(nlohmann::json::parse(buffer_exhausted.body()).at("error").at("message"),
              "buffer capacity is exhausted");
    EXPECT_EQ(buffer_exhausted.find(http::field::retry_after), buffer_exhausted.end());
}

TEST(HttpHandlerTest, SelectsLongestRawPathOnSegmentBoundariesRegardlessOfOrder) {
    apigate::AppConfig config;
    config.routes = {{"/users", {"users", 8001, 100}},
                     {"/users/admin", {"admins", 8002, 200}},
                     {"/orders", {"orders", 8003, 300}},
                     {"/", {"fallback", 8004, 400}}};
    const std::pair<const char*, const char*> cases[] = {
        {"/users", "users"},
        {"/users/42?next=/orders", "users"},
        {"/users/admin", "admins"},
        {"/users/admin/42", "admins"},
        {"/users/admin-extra", "users"},
        {"/users-extra", "fallback"},
        {"/orders/42", "orders"},
        {"/Users", "fallback"},
        {"/u%73ers", "fallback"},
        {"/users%2f42", "fallback"},
        {"/users/../orders", "users"},
        {"/", "fallback"},
    };
    for (int order = 0; order < 2; ++order) {
        for (const auto& [target, host] : cases) {
            const auto request = make_request(http::verb::get, target);
            const auto selected = apigate::select_http_upstream(config, request);
            if (selected) {
                EXPECT_EQ(selected->upstream.host, host);
            } else {
                FAIL() << "expected a matching route";
            }
        }
        std::ranges::reverse(config.routes);
    }
    for (const char* target : {"/healthz?x", "/readyz"}) {
        const auto request = make_request(http::verb::get, target);
        EXPECT_FALSE(apigate::select_http_upstream(config, request).has_value());
        EXPECT_EQ(apigate::handle_http_request(config, request).result(), http::status::ok);
    }
}

TEST(HttpHandlerTest, RouteModeMissesStayLocalAndProtocolErrorsStayConsistent) {
    apigate::AppConfig config;
    config.routes = {{"/users", {"users", 8001, 100}}};
    for (const auto method :
         {http::verb::get, http::verb::post, http::verb::put, http::verb::patch}) {
        const auto miss = make_request(method, "/users-extra?x");
        EXPECT_FALSE(apigate::should_proxy_http_request(config, miss));
        EXPECT_EQ(apigate::handle_http_request(config, miss).result(), http::status::not_found);
        auto hit = make_request(method, "/users/42");
        if (method != http::verb::get) {
            hit.body() = "body";
        }
        const auto selected = apigate::select_http_upstream(config, hit);
        if (selected) {
            EXPECT_EQ(selected->upstream.timeout_ms, 100U);
        } else {
            FAIL() << "expected a matching route";
        }
    }
    auto invalid_get = make_request(http::verb::get, "/unmatched");
    invalid_get.body() = "not allowed";
    auto upgrade = make_request(http::verb::get, "/users");
    upgrade.set(http::field::upgrade, "websocket");
    for (const auto& request :
         {invalid_get, upgrade, make_request(http::verb::get, "http://example.test/users"),
          make_request(http::verb::get, "/users#fragment")}) {
        EXPECT_FALSE(apigate::select_http_upstream(config, request).has_value());
        EXPECT_EQ(apigate::handle_http_request(config, request).result(),
                  http::status::bad_request);
    }
    for (const auto method : {http::verb::head, http::verb::delete_, http::verb::options}) {
        const auto request = make_request(method, "/users");
        EXPECT_FALSE(apigate::select_http_upstream(config, request).has_value());
        EXPECT_EQ(apigate::handle_http_request(config, request).result(),
                  http::status::method_not_allowed);
    }
    const auto legacy = proxy_config();
    const auto selected =
        apigate::select_http_upstream(legacy, make_request(http::verb::post, "/any"));
    if (legacy.upstream && selected) {
        EXPECT_EQ(&selected->upstream, &*legacy.upstream);
        EXPECT_TRUE(selected->prefix.empty());
        EXPECT_FALSE(selected->strip_prefix);
    } else {
        FAIL() << "legacy fixture requires an upstream";
    }
}

}  // namespace
