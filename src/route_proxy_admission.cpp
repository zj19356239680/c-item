#include "route_proxy_admission.hpp"

#include <stdexcept>
#include <utility>

namespace apigate::detail
{

RouteProxyAdmission::RouteProxyAdmission(const std::vector<RouteConfig>& routes,
                                         std::shared_ptr<ProxyAdmissionState> proxies,
                                         TokenBucket::Clock::time_point now)
    : rate_limits_(routes, now), proxies_(std::move(proxies)), next_upstream_(routes.size(), 0)
{
    if (!proxies_)
    {
        throw std::invalid_argument("proxy admission state must not be empty");
    }
}

RouteProxyAdmissionResult RouteProxyAdmission::try_acquire(const SelectedRoute& route,
                                                           TokenBucket::Clock::time_point now)
{
    RouteProxyAdmissionResult result;
    if (route.route_index)
    {
        result.rate_limit = rate_limits_.try_acquire(*route.route_index, now);
        if (!result.rate_limit.allowed)
        {
            return result;
        }
    }
    result.permit = ProxyPermit::try_acquire(proxies_);
    if (!result.permit)
    {
        return result;
    }
    if (route.upstreams.empty() || route.upstreams.size() > 16)
    {
        throw std::logic_error("route requires 1-16 upstream slots");
    }
    std::size_t index = 0;
    if (route.route_index)
    {
        auto& next = next_upstream_.at(*route.route_index);
        if (next >= route.upstreams.size())
        {
            throw std::logic_error("route upstream position is invalid");
        }
        index = next;
        // Bounded position, not a lifetime request counter. Never roll back after selection.
        next = next == route.upstreams.size() - 1 ? 0 : next + 1;
    }
    result.upstream.emplace(SelectedUpstream{route.upstreams[index], route.prefix,
                                             route.strip_prefix, route.route_index});
    return result;
}

}  // namespace apigate::detail
