#include "metrics.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace apigate::detail
{
namespace
{
constexpr std::array<std::string_view, 4> rejection_names{"rate_limit", "proxy_capacity",
                                                          "buffer_capacity", "no_healthy_upstream"};
constexpr std::array<ProxyFailureStage, 8> failure_stages{ProxyFailureStage::resolve,
                                                          ProxyFailureStage::connect,
                                                          ProxyFailureStage::handshake,
                                                          ProxyFailureStage::write,
                                                          ProxyFailureStage::read,
                                                          ProxyFailureStage::timeout,
                                                          ProxyFailureStage::response_too_large,
                                                          ProxyFailureStage::invalid_response};

struct MetricFamily
{
    std::string_view name;
    std::string_view type;
    std::string_view help;
};

class Exposition
{
   public:
    Exposition()
    {
        output_.reserve(Metrics::max_output_bytes);
    }
    void append(std::string_view text)
    {
        if (text.size() > Metrics::max_output_bytes - output_.size())
        {
            throw std::length_error("metrics output exceeds limit");
        }
        output_.append(text);
    }
    template <typename Number>
    void number(Number value)
    {
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc{})
        {
            throw std::runtime_error("metrics number conversion failed");
        }
        append({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }
    void family(const MetricFamily& definition)
    {
        append("# HELP apigate_");
        append(definition.name);
        append(" ");
        append(definition.help);
        append("\n# TYPE apigate_");
        append(definition.name);
        append(" ");
        append(definition.type);
        append("\n");
    }
    void gauge(std::string_view name, std::string_view help, std::uint64_t value)
    {
        family({name, "gauge", help});
        append("apigate_");
        append(name);
        append(" ");
        number(value);
        append("\n");
    }
    [[nodiscard]] std::string take()
    {
        return std::move(output_);
    }

   private:
    std::string output_;
};
}  // namespace

void saturating_increment(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max())
    {
        ++value;
    }
}
void Metrics::response(unsigned int status) noexcept
{
    saturating_increment(responses_[status >= 100 && status <= 599 ? status - 100 : 500]);
}
void Metrics::rejection(RejectionReason reason) noexcept
{
    const auto index = static_cast<std::size_t>(reason);
    if (index < rejections_.size())
    {
        saturating_increment(rejections_[index]);
    }
}
void Metrics::upstream_failure(ProxyFailureStage stage) noexcept
{
    for (std::size_t index = 0; index < failure_stages.size(); ++index)
    {
        if (stage == failure_stages[index])
        {
            saturating_increment(failures_[index]);
            return;
        }
    }
}
void Metrics::processing(double seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds < 0)
    {
        return;
    }
    saturating_increment(processing_count_);
    const auto limit = std::numeric_limits<double>::max();
    processing_sum_ = seconds > limit - processing_sum_ ? limit : processing_sum_ + seconds;
}
std::string Metrics::render(const AppConfig& config, const AcceptCapacityState& connections,
                            const ProxyAdmissionState& proxies, const BufferBudgetState& budget,
                            const UpstreamHealthState& health) const
{
    if (config.routes.size() > 32)
    {
        throw std::length_error("metrics route limit exceeded");
    }
    Exposition out;
    out.family(
        {"http_responses_total", "counter",
         "Final responses generated, not delivery acknowledgements; excludes metrics requests."});
    for (std::size_t index = 0; index < responses_.size(); ++index)
    {
        out.append("apigate_http_responses_total{status=\"");
        if (index == 500)
        {
            out.append("other");
        }
        else
        {
            out.number(index + 100);
        }
        out.append("\"} ");
        out.number(responses_[index]);
        out.append("\n");
    }
    out.family(
        {"rejections_total", "counter", "Local admission rejections; excludes metrics requests."});
    for (std::size_t index = 0; index < rejections_.size(); ++index)
    {
        out.append("apigate_rejections_total{reason=\"");
        out.append(rejection_names[index]);
        out.append("\"} ");
        out.number(rejections_[index]);
        out.append("\n");
    }
    out.family({"upstream_failures_total", "counter",
                "Business proxy failures; excludes health probes and normal cancellation."});
    for (std::size_t index = 0; index < failures_.size(); ++index)
    {
        out.append("apigate_upstream_failures_total{stage=\"");
        out.append(to_string(failure_stages[index]));
        out.append("\"} ");
        out.number(failures_[index]);
        out.append("\n");
    }
    out.family({"request_processing_seconds", "summary",
                "Complete request dispatch to response ready (streaming: header ready); excludes "
                "upload, downstream write, "
                "early parse rejections, cancellation without response and metrics requests. "
                "Population differs from http_responses_total."});
    out.append("apigate_request_processing_seconds_sum ");
    out.number(processing_sum_);
    out.append("\napigate_request_processing_seconds_count ");
    out.number(processing_count_);
    out.append("\n");
    out.gauge("active_connections", "Current downstream connections, including the scraper.",
              connections.active_connections());
    out.gauge("max_connections", "Downstream connection limit.", connections.max_connections());
    out.gauge("active_proxies", "Current business proxy permits.", proxies.active_proxies());
    out.gauge("max_concurrent_proxies", "Business proxy permit limit.", proxies.max_proxies());
    out.gauge("in_flight_body_bytes", "Current business body reservation bytes.",
              budget.current_bytes());
    out.gauge("max_in_flight_body_bytes", "Business body reservation limit, not an RSS limit.",
              budget.max_bytes());
    out.family({"upstream_available", "gauge",
                "Slot eligible for selection; not proof of health or reachability."});
    for (std::size_t route = 0; route < config.routes.size(); ++route)
    {
        const auto slots = config.routes[route].upstreams.size();
        if (slots > 16)
        {
            throw std::length_error("metrics upstream limit exceeded");
        }
        for (std::size_t slot = 0; slot < slots; ++slot)
        {
            out.append("apigate_upstream_available{route_index=\"");
            out.number(route);
            out.append("\",upstream_index=\"");
            out.number(slot);
            out.append("\"} ");
            out.number(health.available(route, slot) ? 1 : 0);
            out.append("\n");
        }
    }
    return out.take();
}

void RequestObservation::exclude(bool excluded) noexcept
{
    excluded_ = excluded;
}
void RequestObservation::begin(Clock::time_point now) noexcept
{
    if (!started_ && !completed_)
    {
        started_ = now;
    }
}
bool RequestObservation::finish(Metrics* metrics, unsigned int status,
                                Clock::time_point now) noexcept
{
    if (completed_)
    {
        return false;
    }
    completed_ = true;
    if (metrics && !excluded_)
    {
        metrics->response(status);
        if (started_)
        {
            const auto start = *started_;
            // Convert before subtracting to avoid overflowing an extreme clock duration.
            const double elapsed = std::chrono::duration<double>(now.time_since_epoch()).count() -
                                   std::chrono::duration<double>(start.time_since_epoch()).count();
            metrics->processing(elapsed);
        }
    }
    return true;
}
void RequestObservation::reject(Metrics* metrics, RejectionReason reason) noexcept
{
    if (metrics && !excluded_ && !completed_ && !rejected_)
    {
        rejected_ = true;
        metrics->rejection(reason);
    }
}
void RequestObservation::fail(Metrics* metrics, ProxyFailureStage stage) noexcept
{
    if (!completed_)
    {
        fail_stream(metrics, stage);
    }
}
void RequestObservation::fail_stream(Metrics* metrics, ProxyFailureStage stage) noexcept
{
    if (metrics && !excluded_ && !failed_)
    {
        failed_ = true;
        metrics->upstream_failure(stage);
    }
}
bool RequestObservation::completed() const noexcept
{
    return completed_;
}
}  // namespace apigate::detail
