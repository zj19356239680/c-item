#include "api_key_auth.hpp"

#include <gtest/gtest.h>
#include <openssl/rand.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "http_proxy.hpp"
#include "route_proxy_admission.hpp"

namespace
{
namespace http = boost::beast::http;
using apigate::detail::authenticate_api_key;

std::string test_key(std::size_t length = 64)
{
    std::vector<unsigned char> bytes(length);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    {
        throw std::runtime_error("test entropy unavailable");
    }
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    std::string result;
    for (const auto byte : bytes)
    {
        result += alphabet[byte % alphabet.size()];
    }
    return result;
}

apigate::ApiKeyAuthConfig single_key_config(std::string_view key)
{
    apigate::ApiKeyAuthConfig config;
    config.digests[0] = apigate::detail::hash_api_key(key);
    config.key_count = 1;
    return config;
}

TEST(ApiKeyAuthTest, ReferenceAndCredentialBoundariesAreStrictAscii)
{
    using apigate::detail::valid_api_key;
    using apigate::detail::valid_api_key_reference;
    EXPECT_TRUE(valid_api_key_reference("APIGATE_AUTH_KEY_A0_"));
    EXPECT_TRUE(valid_api_key_reference("APIGATE_AUTH_KEY_" + std::string(64, 'A')));
    for (const auto& reference :
         {std::string{}, std::string{"APIGATE_AUTH_KEY_"}, std::string{"OTHER_KEY"},
          std::string{"APIGATE_AUTH_KEY_lower"}, std::string{"APIGATE_AUTH_KEY_BAD-NAME"},
          "APIGATE_AUTH_KEY_" + std::string(65, 'A')})
    {
        EXPECT_FALSE(valid_api_key_reference(reference));
    }
    EXPECT_FALSE(valid_api_key(test_key(31)));
    EXPECT_TRUE(valid_api_key(test_key(32)));
    EXPECT_TRUE(valid_api_key(test_key(256)));
    EXPECT_FALSE(valid_api_key(test_key(257)));
    for (const auto character : {' ', ',', '.', '=', '\t', '\n', '\0', static_cast<char>(0x80)})
    {
        auto key = test_key();
        key[0] = character;
        EXPECT_FALSE(valid_api_key(key));
    }
}

TEST(ApiKeyAuthTest, ExactlyOneBearerCredentialAndFixedDigestComparison)
{
    const auto key = test_key();
    const auto configured = single_key_config(key);
    EXPECT_EQ(configured.digests[0].size(), 32U);
    apigate::HttpRequest request{http::verb::get, "/private", 11};
    EXPECT_FALSE(authenticate_api_key(configured, request));
    for (const auto scheme : {"Bearer ", "bearer ", "bEaReR   "})
    {
        request.set(http::field::authorization, std::string{scheme} + key);
        EXPECT_TRUE(authenticate_api_key(configured, request));
    }
    std::string comma_list{"Bearer "};
    comma_list.append(key).append(",").append(key);
    std::string multiple{"Bearer "};
    multiple.append(key).append(" ").append(key);
    for (const auto& value :
         {std::string{}, std::string{"Bearer"}, std::string{"Bearer "}, "Basic " + key,
          "Bearer\t" + key, "Bearer " + test_key(), "Bearer " + test_key(31),
          "Bearer " + test_key(257), comma_list, multiple})
    {
        request.set(http::field::authorization, value);
        EXPECT_FALSE(authenticate_api_key(configured, request));
    }
    request.set(http::field::authorization, "Bearer " + key);
    request.insert("aUtHoRiZaTiOn", "Bearer " + key);
    EXPECT_FALSE(authenticate_api_key(configured, request));
    request.erase(http::field::authorization);
    request.target("/private?api_key=" + key);
    request.set(http::field::cookie, key);
    request.body() = key;
    EXPECT_FALSE(authenticate_api_key(configured, request));
}

TEST(ApiKeyAuthTest, AcceptsEveryConfiguredPositionAndFailsClosedForInvalidCounts)
{
    std::array<std::string, 4> keys{test_key(), test_key(), test_key(), test_key()};
    apigate::ApiKeyAuthConfig config;
    for (const auto& key : keys)
    {
        config.digests[config.key_count++] = apigate::detail::hash_api_key(key);
    }
    apigate::HttpRequest request{http::verb::get, "/private", 11};
    for (const auto& key : keys)
    {
        request.set(http::field::authorization, "Bearer " + key);
        EXPECT_TRUE(authenticate_api_key(config, request));
    }
    request.set(http::field::authorization, "Bearer " + test_key());
    EXPECT_FALSE(authenticate_api_key(config, request));
    request.set(http::field::authorization, "Bearer " + keys.front());
    request.insert(http::field::authorization, "Bearer " + keys.back());
    EXPECT_FALSE(authenticate_api_key(config, request));
    request.erase(http::field::authorization);
    request.set(http::field::authorization, "Bearer " + keys.front());
    config.key_count = 0;
    EXPECT_FALSE(authenticate_api_key(config, request));
    config.key_count = apigate::ApiKeyAuthConfig::max_keys + 1;
    EXPECT_FALSE(authenticate_api_key(config, request));
}

TEST(ApiKeyAuthTest, LongestRouteOwnsRuleAndLocalEndpointsRemainExempt)
{
    apigate::AppConfig config;
    const auto parent = single_key_config(test_key());
    const auto child = single_key_config(test_key());
    config.routes = {
        {"/private", {{"localhost", 1}}, true, std::nullopt, std::nullopt, parent},
        {"/private/public", {{"localhost", 2}}},
        {"/private/child", {{"localhost", 3}}, false, std::nullopt, std::nullopt, child},
        {"/", {{"localhost", 4}}, false, std::nullopt, std::nullopt, parent}};
    const auto selected_auth = [&config](std::string_view target)
    {
        const apigate::HttpRequest request{http::verb::get, target, 11};
        const auto selected = apigate::select_http_route(config, request);
        return selected ? selected->auth : nullptr;
    };
    const auto& parent_setting = config.routes[0].auth;
    const auto& child_setting = config.routes[2].auth;
    if (!parent_setting || !child_setting)
    {
        FAIL() << "expected configured route rules";
        return;
    }
    const auto& parent_rule = *parent_setting;
    const auto& child_rule = *child_setting;
    EXPECT_EQ(selected_auth("/private/item"), &parent_rule);
    EXPECT_EQ(selected_auth("/private/public/item"), nullptr);
    EXPECT_EQ(selected_auth("/private/child/item"), &child_rule);
    EXPECT_EQ(selected_auth("/healthz"), nullptr);
    EXPECT_EQ(selected_auth("/readyz"), nullptr);
    config.metrics_enabled = true;
    EXPECT_EQ(selected_auth("/metrics?x=1"), nullptr);
    config.metrics_enabled = false;
    EXPECT_NE(selected_auth("/metrics"), nullptr);
}

TEST(ApiKeyAuthTest, ProtectedSelectionStripsAuthorizationWhilePublicAndLegacyPreserveIt)
{
    const auto key = test_key();
    apigate::AppConfig config;
    config.routes = {
        {"/private", {{"localhost", 1}}, true, std::nullopt, std::nullopt, single_key_config(key)}};
    apigate::HttpRequest request{http::verb::post, "/private/item?raw=%2f", 11};
    request.body() = "payload";
    request.set(http::field::authorization, "Bearer " + key);
    request.set(http::field::connection, "keep-alive, X-Hop");
    request.set("X-Hop", "remove");
    const auto selected = apigate::select_http_route(config, request);
    if (!selected)
    {
        FAIL() << "expected protected route";
        return;
    }
    const auto& route = *selected;
    ASSERT_NE(route.auth, nullptr);
    EXPECT_TRUE(authenticate_api_key(*route.auth, request));
    auto proxies = std::make_shared<apigate::detail::ProxyAdmissionState>(1);
    apigate::detail::RouteProxyAdmission admission{config.routes, proxies};
    auto result = admission.try_acquire(route, apigate::detail::TokenBucket::Clock::now());
    const auto& selected_backend = result.upstream;
    if (!selected_backend)
    {
        FAIL() << "expected admitted backend";
        return;
    }
    const auto& backend = *selected_backend;
    EXPECT_TRUE(backend.strip_authorization);
    const auto outgoing = apigate::detail::make_upstream_request(backend, request);
    EXPECT_EQ(outgoing.count(http::field::authorization), 0U);
    EXPECT_EQ(outgoing.count("X-Hop"), 0U);
    EXPECT_EQ(outgoing.target(), "/item?raw=%2f");
    EXPECT_EQ(outgoing.body(), "payload");
    const auto legacy =
        apigate::detail::make_upstream_request({config.routes[0].upstreams[0], {}}, request);
    EXPECT_EQ(legacy.count(http::field::authorization), 1U);
    auto failure = apigate::make_authentication_failed_response(request);
    EXPECT_EQ(failure.result_int(), 401U);
    EXPECT_EQ(failure[http::field::www_authenticate], "Bearer realm=\"ApiGate\"");
    EXPECT_EQ(failure[http::field::cache_control], "no-store");
    EXPECT_EQ(failure.body().find(key), std::string::npos);
}
}  // namespace
