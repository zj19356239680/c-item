#include <algorithm>
#include <stdexcept>

#include "health_check.hpp"

namespace apigate::detail
{

UpstreamHealthState::UpstreamHealthState(const std::vector<RouteConfig>& routes)
{
    for (const auto& route : routes)
    {
        routes_.push_back({route.health_check, std::vector<Slot>(route.upstreams.size())});
    }
}

bool UpstreamHealthState::available(std::size_t route, std::size_t upstream) const
{
    return routes_.at(route).slots.at(upstream).healthy;
}

HealthTransition UpstreamHealthState::record(std::size_t route, std::size_t upstream, bool success)
{
    auto& slot = routes_.at(route).slots.at(upstream);
    const auto& config = routes_.at(route).config;
    if (!config)
    {
        return HealthTransition::unchanged;
    }
    const auto& settings = *config;
    if (success)
    {
        slot.failures = 0;
        slot.successes = std::min(slot.successes + 1, settings.healthy_threshold);
        if (!slot.healthy && slot.successes == settings.healthy_threshold)
        {
            slot.healthy = true;
            return HealthTransition::healthy;
        }
    }
    else
    {
        slot.successes = 0;
        slot.failures = std::min(slot.failures + 1, settings.unhealthy_threshold);
        if (slot.healthy && slot.failures == settings.unhealthy_threshold)
        {
            slot.healthy = false;
            return HealthTransition::unhealthy;
        }
    }
    return HealthTransition::unchanged;
}

ProbeSchedule::ProbeSchedule(const std::vector<std::chrono::milliseconds>& intervals,
                             Clock::time_point now)
{
    for (const auto interval : intervals)
    {
        if (interval.count() < 100 || interval.count() > 60000)
        {
            throw std::invalid_argument("probe interval must be positive");
        }
        slots_.push_back({interval, now, false});
    }
}

std::optional<std::size_t> ProbeSchedule::take_due(Clock::time_point now)
{
    if (stopped_ || active_ == max_in_flight)
    {
        return std::nullopt;
    }
    for (std::size_t visited = 0; visited < slots_.size(); ++visited)
    {
        const auto index = cursor_;
        cursor_ = cursor_ + 1 == slots_.size() ? 0 : cursor_ + 1;
        auto& slot = slots_[index];
        if (!slot.in_flight && slot.due <= now)
        {
            slot.in_flight = true;
            slot.due = now > Clock::time_point::max() - slot.interval ? Clock::time_point::max()
                                                                      : now + slot.interval;
            ++active_;
            return index;
        }
    }
    return std::nullopt;
}

void ProbeSchedule::complete(std::size_t slot)
{
    auto& entry = slots_.at(slot);
    if (entry.in_flight)
    {
        entry.in_flight = false;
        --active_;
    }
}

std::optional<ProbeSchedule::Clock::time_point> ProbeSchedule::next_due() const
{
    if (stopped_ || active_ == max_in_flight)
    {
        return std::nullopt;
    }
    std::optional<Clock::time_point> next;
    for (const auto& slot : slots_)
    {
        if (!slot.in_flight && (!next || slot.due < *next))
        {
            next = slot.due;
        }
    }
    return next;
}

std::size_t ProbeSchedule::active() const noexcept
{
    return active_;
}
void ProbeSchedule::stop() noexcept
{
    stopped_ = true;
    for (auto& slot : slots_)
    {
        slot.in_flight = false;
    }
    active_ = 0;
}

}  // namespace apigate::detail
