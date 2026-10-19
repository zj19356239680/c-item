#ifndef APIGATE_RATE_LIMIT_HPP_
#define APIGATE_RATE_LIMIT_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "apigate/config.hpp"

namespace apigate::detail
{

struct RateLimitDecision
{
    bool allowed{true};
    std::uint32_t retry_after_seconds{0};
};

// Mutable state is serialized by the single io_context thread. No token refunds.
class TokenBucket
{
   public:
    using Clock = std::chrono::steady_clock;

    TokenBucket(RateLimitConfig config, Clock::time_point now);
    [[nodiscard]] RateLimitDecision try_acquire(Clock::time_point now) noexcept;

   private:
    void refill(Clock::time_point now) noexcept;

    RateLimitConfig config_;
    std::uint64_t capacity_;
    std::uint64_t units_;
    Clock::time_point last_refill_;
};

class RouteRateLimits
{
   public:
    explicit RouteRateLimits(const std::vector<RouteConfig>& routes,
                             TokenBucket::Clock::time_point now = TokenBucket::Clock::now());
    [[nodiscard]] RateLimitDecision try_acquire(std::size_t route_index,
                                                TokenBucket::Clock::time_point now);

   private:
    std::vector<std::optional<TokenBucket>> buckets_;
};

}  // namespace apigate::detail

#endif  // APIGATE_RATE_LIMIT_HPP_
