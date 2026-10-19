#include "route_proxy_admission.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>

namespace
{

using apigate::detail::ProxyAdmissionState;
using apigate::detail::RouteProxyAdmission;
using Clock = apigate::detail::TokenBucket::Clock;
using namespace std::chrono_literals;
namespace http = boost::beast::http;

TEST(RouteProxyAdmissionTest, CyclesBoundedPositionsIndependentlyAndRestartStartsAtFirstSlot)
{
    const Clock::time_point now{};
    apigate::AppConfig config;
    config.routes = {{"/a", {{"same", 1, 100}, {"same", 2, 200}, {"same", 3, 300}}, true},
                     {"/b", {{"same", 1}, {"same", 2}}},
                     {"/single", {{"same", 4}}}};
    auto proxies = std::make_shared<ProxyAdmissionState>(1);
    RouteProxyAdmission state{config.routes, proxies, now};
    const auto route = apigate::select_http_route(config, {http::verb::get, "/a/42?x", 11});
    if (!route)
    {
        FAIL() << "expected selected route";
        return;
    }
    for (int lookup = 0; lookup < 5; ++lookup)
    {
        EXPECT_TRUE(apigate::select_http_route(config, {http::verb::get, "/a/42", 11}).has_value());
    }
    for (int request = 0; request < 1000; ++request)
    {
        auto admitted = state.try_acquire(*route, now);
        EXPECT_TRUE(admitted.rate_limit.allowed);
        if (admitted.upstream && admitted.permit)
        {
            EXPECT_EQ(admitted.upstream->upstream.port, request % 3 + 1);
            EXPECT_EQ(admitted.upstream->upstream.timeout_ms,
                      static_cast<std::uint32_t>(request % 3 + 1) * 100U);
            EXPECT_EQ(admitted.upstream->prefix, "/a");
            EXPECT_TRUE(admitted.upstream->strip_prefix);
            EXPECT_EQ(proxies->active_proxies(), 1U);
        }
        else
        {
            FAIL() << "expected proxy admission";
        }
    }
    const apigate::SelectedRoute other{config.routes[1].upstreams, "/b", false, 1};
    {
        auto admitted = state.try_acquire(other, now);
        if (admitted.upstream)
        {
            EXPECT_EQ(admitted.upstream->upstream.port, 1);
        }
        else
        {
            FAIL() << "expected independent route admission";
        }
    }
    const apigate::SelectedRoute single{config.routes[2].upstreams, "/single", false, 2};
    for (int request = 0; request < 4; ++request)
    {
        auto admitted = state.try_acquire(single, now);
        if (admitted.upstream)
        {
            EXPECT_EQ(admitted.upstream->upstream.port, 4);
        }
        else
        {
            FAIL() << "expected single upstream admission";
        }
    }
    RouteProxyAdmission restarted{config.routes, proxies, now};
    auto first = restarted.try_acquire(*route, now);
    if (first.upstream)
    {
        EXPECT_EQ(first.upstream->upstream.port, 1);
    }
    else
    {
        FAIL() << "expected fresh state to start at the first upstream";
    }
}

TEST(RouteProxyAdmissionTest, RateAndCapacityRejectionDoNotAdvanceAndTokensAreNotRefunded)
{
    const Clock::time_point now{};
    const std::vector<apigate::RouteConfig> routes = {
        {"/a", {{"same", 1}, {"same", 2}, {"same", 3}}, true, apigate::RateLimitConfig{1, 2}}};
    auto proxies = std::make_shared<ProxyAdmissionState>(1);
    RouteProxyAdmission state{routes, proxies, now};
    const apigate::SelectedRoute route{routes.front().upstreams, "/a", true, 0};
    auto first = state.try_acquire(route, now);
    ASSERT_TRUE(first.permit.has_value());
    ASSERT_TRUE(first.upstream.has_value());
    auto capacity_denied = state.try_acquire(route, now);
    EXPECT_TRUE(capacity_denied.rate_limit.allowed);
    EXPECT_FALSE(capacity_denied.permit.has_value());
    EXPECT_FALSE(capacity_denied.upstream.has_value());
    auto rate_denied = state.try_acquire(route, now);
    EXPECT_FALSE(rate_denied.rate_limit.allowed);
    EXPECT_FALSE(rate_denied.permit.has_value());
    EXPECT_FALSE(rate_denied.upstream.has_value());
    first.permit.reset();
    EXPECT_EQ(proxies->active_proxies(), 0U);
    {
        auto second = state.try_acquire(route, now + 1s);
        if (second.upstream)
        {
            EXPECT_EQ(second.upstream->upstream.port, 2);
        }
        else
        {
            FAIL() << "rejections must not advance the position";
        }
    }  // Dropping an admitted exchange (failure/cancel) returns capacity, not its token or
       // position.
    EXPECT_FALSE(state.try_acquire(route, now + 1s).rate_limit.allowed);
    auto third = state.try_acquire(route, now + 2s);
    if (third.upstream)
    {
        EXPECT_EQ(third.upstream->upstream.port, 3);
    }
    else
    {
        FAIL() << "admitted exchanges must advance exactly once";
    }
}

TEST(RouteProxyAdmissionTest, LegacyModeAndExceptionalConfigurationDoNotLeakCapacity)
{
    const Clock::time_point now{};
    auto proxies = std::make_shared<ProxyAdmissionState>(1);
    RouteProxyAdmission state{{}, proxies, now};
    EXPECT_THROW((RouteProxyAdmission{{}, nullptr, now}), std::invalid_argument);
    const apigate::SelectedRoute invalid{{}, {}, false};
    EXPECT_THROW(static_cast<void>(state.try_acquire(invalid, now)), std::logic_error);
    EXPECT_EQ(proxies->active_proxies(), 0U);
    apigate::AppConfig config;
    config.upstream = apigate::UpstreamConfig{"legacy", 80};
    const auto route = apigate::select_http_route(config, {http::verb::get, "/any", 11});
    if (!route)
    {
        FAIL() << "expected legacy route";
        return;
    }
    auto admitted = state.try_acquire(*route, now);
    if (admitted.upstream)
    {
        EXPECT_EQ(admitted.upstream->upstream.host, "legacy");
    }
    else
    {
        FAIL() << "expected legacy backend";
    }
}

}  // namespace
