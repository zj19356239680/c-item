#include "rate_limit.hpp"

#include <stdexcept>
#include <type_traits>

namespace apigate::detail
{
namespace
{

// Fixed-point nanotokens: capacity <= 10^15, with no floating-point drift.
constexpr std::uint64_t units_per_token = 1000000000;

[[nodiscard]] std::uint64_t ceil_divide(std::uint64_t numerator, std::uint64_t denominator) noexcept
{
    return numerator / denominator + (numerator % denominator != 0 ? 1U : 0U);
}

}  // namespace

TokenBucket::TokenBucket(RateLimitConfig config, Clock::time_point now)
    : config_(config),
      capacity_(static_cast<std::uint64_t>(config.burst) * units_per_token),
      units_(capacity_),
      last_refill_(now)
{
    if (config.requests_per_second == 0 || config.requests_per_second > 1000000 ||
        config.burst == 0 || config.burst > 1000000)
    {
        throw std::invalid_argument("rate limit values must be integers from 1 to 1000000");
    }
}

void TokenBucket::refill(Clock::time_point now) noexcept
{
    if (now <= last_refill_)
    {
        return;
    }
    // Unsigned subtraction also handles a test clock spanning signed min/max without UB.
    using UnsignedTicks = std::make_unsigned_t<Clock::duration::rep>;
    const auto ticks = static_cast<UnsignedTicks>(now.time_since_epoch().count()) -
                       static_cast<UnsignedTicks>(last_refill_.time_since_epoch().count());
    const std::chrono::duration<UnsignedTicks, Clock::period> elapsed{ticks};
    last_refill_ = now;
    // Even at the minimum rate this fills the bucket; clamp before conversion/multiplication.
    if (elapsed >= std::chrono::seconds{config_.burst})
    {
        units_ = capacity_;
        return;
    }
    const auto nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const auto missing = capacity_ - units_;
    if (nanoseconds >= ceil_divide(missing, config_.requests_per_second))
    {
        units_ = capacity_;
    }
    else
    {
        // This product is strictly smaller than missing, which is at most 10^15.
        units_ += nanoseconds * config_.requests_per_second;
    }
}

RateLimitDecision TokenBucket::try_acquire(Clock::time_point now) noexcept
{
    refill(now);
    if (units_ >= units_per_token)
    {
        units_ -= units_per_token;
        return {};
    }
    const auto wait_nanoseconds =
        ceil_divide(units_per_token - units_, config_.requests_per_second);
    return {false, static_cast<std::uint32_t>(ceil_divide(wait_nanoseconds, units_per_token))};
}

RouteRateLimits::RouteRateLimits(const std::vector<RouteConfig>& routes,
                                 TokenBucket::Clock::time_point now)
{
    buckets_.reserve(routes.size());
    for (const auto& route : routes)
    {
        if (route.rate_limit)
        {
            buckets_.emplace_back(std::in_place, *route.rate_limit, now);
        }
        else
        {
            buckets_.emplace_back(std::nullopt);
        }
    }
}

RateLimitDecision RouteRateLimits::try_acquire(std::size_t route_index,
                                               TokenBucket::Clock::time_point now)
{
    auto& bucket = buckets_.at(route_index);
    return bucket ? bucket->try_acquire(now) : RateLimitDecision{};
}

}  // namespace apigate::detail
