#ifndef APIGATE_HEALTH_CHECK_HPP_
#define APIGATE_HEALTH_CHECK_HPP_

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "apigate/config.hpp"

namespace apigate
{
class StructuredLogger;
}

namespace apigate::detail
{

enum class HealthTransition : std::uint8_t
{
    unchanged,
    unhealthy,
    healthy
};

// Server-owned state, shared with admission; serialized by the single io_context.
class UpstreamHealthState
{
   public:
    explicit UpstreamHealthState(const std::vector<RouteConfig>& routes);
    [[nodiscard]] bool available(std::size_t route, std::size_t upstream) const;
    HealthTransition record(std::size_t route, std::size_t upstream, bool success);

   private:
    struct Slot
    {
        bool healthy{true};
        std::uint32_t failures{0};
        std::uint32_t successes{0};
    };
    struct Route
    {
        std::optional<HealthCheckConfig> config;
        std::vector<Slot> slots;
    };
    std::vector<Route> routes_;
};

// Bounded, rotating due-slot selection. No queued work beyond configured slots.
class ProbeSchedule
{
   public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t max_in_flight = 4;
    ProbeSchedule(const std::vector<std::chrono::milliseconds>& intervals, Clock::time_point now);
    [[nodiscard]] std::optional<std::size_t> take_due(Clock::time_point now);
    void complete(std::size_t slot);
    [[nodiscard]] std::optional<Clock::time_point> next_due() const;
    [[nodiscard]] std::size_t active() const noexcept;
    void stop() noexcept;

   private:
    struct Slot
    {
        std::chrono::milliseconds interval;
        Clock::time_point due;
        bool in_flight{false};
    };
    std::vector<Slot> slots_;
    std::size_t cursor_{0};
    std::size_t active_{0};
    bool stopped_{false};
};

class HealthChecks
{
   public:
    HealthChecks(const boost::asio::any_io_executor& executor, const AppConfig& config,
                 std::shared_ptr<UpstreamHealthState> state, StructuredLogger& logger,
                 std::function<void()> on_failure);
    ~HealthChecks();
    HealthChecks(const HealthChecks&) = delete;
    HealthChecks& operator=(const HealthChecks&) = delete;
    void start();
    void stop() noexcept;

   private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace apigate::detail

#endif  // APIGATE_HEALTH_CHECK_HPP_
