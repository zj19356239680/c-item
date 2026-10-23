#ifndef APIGATE_METRICS_HPP_
#define APIGATE_METRICS_HPP_

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "buffer_budget.hpp"
#include "health_check.hpp"
#include "http_proxy.hpp"
#include "http_server_internal.hpp"

namespace apigate::detail
{

enum class RejectionReason : std::uint8_t
{
    rate_limit,
    proxy_capacity,
    buffer_capacity,
    no_healthy_upstream,
};

// Fixed storage, serialized by the server's single io_context. No input-derived labels.
class Metrics
{
   public:
    static constexpr std::size_t max_output_bytes = std::size_t{128} * 1024;
    void response(unsigned int status) noexcept;
    void rejection(RejectionReason reason) noexcept;
    void upstream_failure(ProxyFailureStage stage) noexcept;
    void processing(double seconds) noexcept;
    [[nodiscard]] std::string render(const AppConfig& config,
                                     const AcceptCapacityState& connections,
                                     const ProxyAdmissionState& proxies,
                                     const BufferBudgetState& budget,
                                     const UpstreamHealthState& health) const;

   private:
    std::array<std::uint64_t, 501> responses_{};
    std::array<std::uint64_t, 4> rejections_{};
    std::array<std::uint64_t, 8> failures_{};
    std::uint64_t processing_count_{0};
    double processing_sum_{0};
};

void saturating_increment(std::uint64_t& value) noexcept;

// One instance per session; reset only before reading the next request.
class RequestObservation
{
   public:
    using Clock = std::chrono::steady_clock;
    void exclude(bool excluded) noexcept;
    void begin(Clock::time_point now) noexcept;
    [[nodiscard]] bool finish(Metrics* metrics, unsigned int status,
                              Clock::time_point now) noexcept;
    void reject(Metrics* metrics, RejectionReason reason) noexcept;
    void fail(Metrics* metrics, ProxyFailureStage stage) noexcept;
    [[nodiscard]] bool completed() const noexcept;

   private:
    std::optional<Clock::time_point> started_;
    bool excluded_{false};
    bool completed_{false};
    bool rejected_{false};
    bool failed_{false};
};

}  // namespace apigate::detail
#endif  // APIGATE_METRICS_HPP_
