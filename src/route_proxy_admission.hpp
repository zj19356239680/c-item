#ifndef APIGATE_ROUTE_PROXY_ADMISSION_HPP_
#define APIGATE_ROUTE_PROXY_ADMISSION_HPP_

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "apigate/http_handler.hpp"
#include "http_server_internal.hpp"
#include "rate_limit.hpp"

namespace apigate::detail
{

struct RouteProxyAdmissionResult
{
    RateLimitDecision rate_limit;
    std::optional<ProxyPermit> permit;
    // Borrows immutable configuration; proxy construction consumes it synchronously.
    std::optional<SelectedUpstream> upstream;
};

// One server-owned instance shared by all sessions, serialized by the single io_context.
class RouteProxyAdmission
{
   public:
    RouteProxyAdmission(const std::vector<RouteConfig>& routes,
                        std::shared_ptr<ProxyAdmissionState> proxies,
                        TokenBucket::Clock::time_point now = TokenBucket::Clock::now());
    [[nodiscard]] RouteProxyAdmissionResult try_acquire(const SelectedRoute& route,
                                                        TokenBucket::Clock::time_point now);

   private:
    RouteRateLimits rate_limits_;
    std::shared_ptr<ProxyAdmissionState> proxies_;
    std::vector<std::size_t> next_upstream_;
};

}  // namespace apigate::detail

#endif  // APIGATE_ROUTE_PROXY_ADMISSION_HPP_
