#include "rate_limit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace
{

using apigate::detail::TokenBucket;
using Clock = TokenBucket::Clock;
using namespace std::chrono_literals;

TEST(TokenBucketTest, StartsWithBurstAndRejectsWithoutRefunds)
{
    const Clock::time_point now{};
    TokenBucket bucket{{1, 3}, now};
    for (int request = 0; request < 3; ++request)
    {
        EXPECT_TRUE(bucket.try_acquire(now).allowed);
    }
    for (int request = 0; request < 3; ++request)
    {
        const auto decision = bucket.try_acquire(now);
        EXPECT_FALSE(decision.allowed);
        EXPECT_EQ(decision.retry_after_seconds, 1U);
    }
}

TEST(TokenBucketTest, PreservesFractionalRefillsAndRoundsRetryAfterUp)
{
    const Clock::time_point now{};
    TokenBucket bucket{{3, 1}, now};
    EXPECT_TRUE(bucket.try_acquire(now).allowed);
    EXPECT_FALSE(bucket.try_acquire(now + 100ms).allowed);
    EXPECT_FALSE(bucket.try_acquire(now + 200ms).allowed);
    const auto almost = bucket.try_acquire(now + 333333333ns);
    EXPECT_FALSE(almost.allowed);
    EXPECT_EQ(almost.retry_after_seconds, 1U);
    const auto ready = bucket.try_acquire(now + 333333334ns);
    EXPECT_TRUE(ready.allowed);
    EXPECT_EQ(ready.retry_after_seconds, 0U);
    EXPECT_FALSE(bucket.try_acquire(now + 666666666ns).allowed);
    EXPECT_TRUE(bucket.try_acquire(now + 666666668ns).allowed);
}

TEST(TokenBucketTest, CapsAtBurstAfterLongIdleAndIgnoresBackwardTime)
{
    const Clock::time_point now{};
    TokenBucket bucket{{1, 2}, now};
    EXPECT_TRUE(bucket.try_acquire(now).allowed);
    EXPECT_TRUE(bucket.try_acquire(now).allowed);
    EXPECT_FALSE(bucket.try_acquire(now - 1ns).allowed);
    EXPECT_FALSE(bucket.try_acquire(now + 999999999ns).allowed);
    EXPECT_TRUE(bucket.try_acquire(now + 1s).allowed);
    const auto later = now + 24h;
    EXPECT_TRUE(bucket.try_acquire(later).allowed);
    EXPECT_TRUE(bucket.try_acquire(later).allowed);
    EXPECT_FALSE(bucket.try_acquire(later).allowed);
}

TEST(TokenBucketTest, HandlesMaximumRateBurstAndExtremeClockSpanWithoutOverflow)
{
    const auto now = Clock::time_point::min();
    TokenBucket bucket{{1000000, 1000000}, now};
    std::uint32_t admitted = 0;
    for (std::uint32_t request = 0; request < 1000001; ++request)
    {
        admitted += bucket.try_acquire(now).allowed ? 1U : 0U;
    }
    EXPECT_EQ(admitted, 1000000U);
    EXPECT_FALSE(bucket.try_acquire(now + 999ns).allowed);
    EXPECT_TRUE(bucket.try_acquire(now + 1000ns).allowed);
    EXPECT_TRUE(bucket.try_acquire(Clock::time_point::max()).allowed);
    EXPECT_TRUE(bucket.try_acquire(Clock::time_point::max()).allowed);
    TokenBucket minimum{{1, 1}, now};
    EXPECT_TRUE(minimum.try_acquire(now).allowed);
    EXPECT_TRUE(minimum.try_acquire(Clock::time_point::max()).allowed);
    EXPECT_FALSE(minimum.try_acquire(Clock::time_point::max()).allowed);
    EXPECT_FALSE(minimum.try_acquire(now).allowed);
}

TEST(TokenBucketTest, RejectsInvalidProgrammaticConfiguration)
{
    const Clock::time_point now{};
    for (const auto config : {apigate::RateLimitConfig{0, 1}, {1, 0}, {1000001, 1}, {1, 1000001}})
    {
        EXPECT_THROW((TokenBucket{config, now}), std::invalid_argument);
    }
}

TEST(RouteRateLimitsTest, RoutesWithTheSameBackendHaveIndependentSharedStateAndDefaultIsUnlimited)
{
    const Clock::time_point now{};
    const std::vector<apigate::RouteConfig> routes = {
        {"/a", {{"same", 80}}, true, apigate::RateLimitConfig{1, 1}},
        {"/b", {{"same", 80}}, false, apigate::RateLimitConfig{2, 1}},
        {"/unlimited", {{"same", 80}}}};
    apigate::detail::RouteRateLimits limits{routes, now};
    EXPECT_TRUE(limits.try_acquire(0, now).allowed);
    EXPECT_FALSE(limits.try_acquire(0, now).allowed);
    EXPECT_TRUE(limits.try_acquire(1, now).allowed);
    EXPECT_FALSE(limits.try_acquire(1, now).allowed);
    EXPECT_FALSE(limits.try_acquire(0, now + 500ms).allowed);
    EXPECT_TRUE(limits.try_acquire(1, now + 500ms).allowed);
    for (int request = 0; request < 5; ++request)
    {
        EXPECT_TRUE(limits.try_acquire(2, now).allowed);
    }
    EXPECT_THROW(static_cast<void>(limits.try_acquire(3, now)), std::out_of_range);
}

}  // namespace
