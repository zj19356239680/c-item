#include "health_check.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <vector>

#include "route_proxy_admission.hpp"

namespace
{
using namespace std::chrono_literals;
using apigate::detail::HealthTransition;
using apigate::detail::ProbeSchedule;
using apigate::detail::UpstreamHealthState;
using Clock = ProbeSchedule::Clock;

std::vector<apigate::RouteConfig> routes()
{
    return {{"/a",
             {{"same", 1}, {"same", 1}},
             false,
             std::nullopt,
             apigate::HealthCheckConfig{"/health", 5000, 1000, 3, 2}},
            {"/disabled", {{"same", 1}}}};
}

TEST(UpstreamHealthTest, StartsAvailableKeepsSlotsIndependentAndLeavesDisabledRoutesAlone)
{
    UpstreamHealthState state{routes()};
    EXPECT_TRUE(state.available(0, 0));
    EXPECT_EQ(state.record(0, 0, false), HealthTransition::unchanged);
    EXPECT_EQ(state.record(0, 0, false), HealthTransition::unchanged);
    EXPECT_EQ(state.record(0, 0, false), HealthTransition::unhealthy);
    EXPECT_FALSE(state.available(0, 0));
    EXPECT_TRUE(state.available(0, 1));
    for (int count = 0; count < 1000; ++count)
    {
        EXPECT_EQ(state.record(0, 0, false), HealthTransition::unchanged);
        EXPECT_EQ(state.record(1, 0, false), HealthTransition::unchanged);
    }
    EXPECT_TRUE(state.available(1, 0));
    EXPECT_EQ(state.record(0, 0, true), HealthTransition::unchanged);
    EXPECT_EQ(state.record(0, 0, true), HealthTransition::healthy);
    for (int count = 0; count < 1000; ++count)
    {
        EXPECT_EQ(state.record(0, 0, true), HealthTransition::unchanged);
    }
    UpstreamHealthState restarted{routes()};
    EXPECT_TRUE(restarted.available(0, 0));
}

TEST(UpstreamHealthTest, InterleavedResultsResetFailureAndRecoverySequences)
{
    UpstreamHealthState state{routes()};
    for (int count = 0; count < 2; ++count)
    {
        state.record(0, 0, false);
    }
    state.record(0, 0, true);
    for (int count = 0; count < 2; ++count)
    {
        EXPECT_EQ(state.record(0, 0, false), HealthTransition::unchanged);
    }
    EXPECT_EQ(state.record(0, 0, false), HealthTransition::unhealthy);
    EXPECT_EQ(state.record(0, 0, true), HealthTransition::unchanged);
    state.record(0, 0, false);
    EXPECT_EQ(state.record(0, 0, true), HealthTransition::unchanged);
    EXPECT_FALSE(state.available(0, 0));
    EXPECT_EQ(state.record(0, 0, true), HealthTransition::healthy);
}

TEST(ProbeScheduleTest, CapsConcurrencyDoesNotDuplicateAndRotatesPastBusySlotsFairly)
{
    const Clock::time_point now{};
    ProbeSchedule schedule{std::vector(9, 100ms), now};
    for (std::size_t slot = 0; slot < 4; ++slot)
    {
        EXPECT_EQ(schedule.take_due(now), slot);
    }
    EXPECT_EQ(schedule.active(), 4U);
    EXPECT_FALSE(schedule.take_due(now + 1s));
    EXPECT_FALSE(schedule.next_due());  // no overdue timer spin while saturated
    for (std::size_t slot = 0; slot < 5; ++slot)
    {
        schedule.complete(slot);
        EXPECT_EQ(schedule.take_due(now + 1s), slot + 4);
    }
    schedule.stop();
    EXPECT_EQ(schedule.active(), 0U);
    schedule.complete(8);
    schedule.complete(8);
    EXPECT_EQ(schedule.active(), 0U);
    EXPECT_FALSE(schedule.take_due(now + 100s));
    EXPECT_FALSE(schedule.next_due());
}

TEST(ProbeScheduleTest, StartsImmediatelyButPreservesIntervalWithoutCatchupQueue)
{
    const Clock::time_point now{};
    ProbeSchedule schedule{{100ms, 200ms}, now};
    EXPECT_EQ(schedule.take_due(now), 0U);
    EXPECT_EQ(schedule.take_due(now), 1U);
    schedule.complete(0);
    schedule.complete(0);
    EXPECT_EQ(schedule.active(), 1U);
    EXPECT_EQ(schedule.next_due(), now + 100ms);
    EXPECT_FALSE(schedule.take_due(now + 99ms));
    EXPECT_EQ(schedule.take_due(now + 100ms), 0U);
    schedule.complete(1);
    EXPECT_EQ(schedule.next_due(), now + 200ms);
    EXPECT_EQ(schedule.take_due(now + 1h), 1U);
    EXPECT_FALSE(schedule.take_due(now + 1h));  // neither in-flight slot is duplicated
    ProbeSchedule empty{{}, now};
    EXPECT_FALSE(empty.take_due(now));
    EXPECT_FALSE(empty.next_due());
    ProbeSchedule boundary{{100ms}, Clock::time_point::max() - 1ms};
    EXPECT_EQ(boundary.take_due(Clock::time_point::max() - 1ms), 0U);
    boundary.complete(0);
    EXPECT_EQ(boundary.next_due(), Clock::time_point::max());
}

TEST(HealthyAdmissionTest, SkipsEjectedSlotsRejoinsAndAllDownReleasesPermitWithoutMovingPosition)
{
    apigate::AppConfig config;
    config.routes = routes();
    config.routes[0].health_check = apigate::HealthCheckConfig{"/health", 5000, 1000, 1, 1};
    config.routes[0].rate_limit = apigate::RateLimitConfig{1, 6};
    const Clock::time_point now{};
    auto proxies = std::make_shared<apigate::detail::ProxyAdmissionState>(1);
    apigate::detail::RouteProxyAdmission admission{config.routes, proxies, now};
    const auto selected =
        apigate::select_http_route(config, {boost::beast::http::verb::get, "/a", 11});
    if (!selected)
    {
        FAIL() << "expected selected route";
        return;
    }
    auto health = admission.health_state();
    EXPECT_EQ(health->record(0, 0, false), HealthTransition::unhealthy);
    {
        auto result = admission.try_acquire(*selected, now);
        if (result.upstream)
        {
            EXPECT_EQ(&result.upstream->upstream, &config.routes[0].upstreams[1]);
        }
        else
        {
            FAIL() << "expected healthy slot one";
        }
    }
    EXPECT_EQ(health->record(0, 1, false), HealthTransition::unhealthy);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        auto rejected = admission.try_acquire(*selected, now);
        EXPECT_TRUE(rejected.rate_limit.allowed);
        EXPECT_TRUE(rejected.no_healthy_upstream);
        EXPECT_FALSE(rejected.permit);
        EXPECT_FALSE(rejected.upstream);
        EXPECT_EQ(proxies->active_proxies(), 0U);
    }
    health->record(0, 0, true);
    health->record(0, 1, true);
    {
        auto restored = admission.try_acquire(*selected, now);
        // The all-down decisions did not advance the position, so return to slot zero.
        if (restored.upstream)
        {
            EXPECT_EQ(&restored.upstream->upstream, &config.routes[0].upstreams[0]);
        }
        else
        {
            FAIL() << "expected restored slot zero";
        }
    }
    auto response = apigate::make_gateway_error_response(
        {boost::beast::http::verb::post, "/a", 11}, apigate::GatewayFailure::no_healthy_upstream);
    EXPECT_EQ(response.result_int(), 503U);
    EXPECT_EQ(
        response.body(),
        R"({"error":{"code":"no_healthy_upstream","message":"no healthy upstream is available"}})");
    EXPECT_FALSE(response.count(boost::beast::http::field::retry_after));
    static_cast<void>(admission.try_acquire(*selected, now));
    EXPECT_FALSE(
        admission.try_acquire(*selected, now).rate_limit.allowed);  // no refund on all-down
}

}  // namespace
