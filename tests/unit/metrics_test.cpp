#include "metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace
{
using namespace std::chrono_literals;
using apigate::detail::Metrics;
using apigate::detail::RejectionReason;
using apigate::detail::RequestObservation;

std::string render(const Metrics& metrics, const apigate::AppConfig& config = {})
{
    return metrics.render(config, apigate::detail::AcceptCapacityState{256},
                          apigate::detail::ProxyAdmissionState{32},
                          apigate::detail::BufferBudgetState{67108864},
                          apigate::detail::UpstreamHealthState{config.routes});
}

TEST(MetricsTest, ObservesOnlyFinalResponsesOnceAndTimesOnlyDispatchedRequests)
{
    Metrics metrics;
    RequestObservation request;
    const RequestObservation::Clock::time_point now{};
    request.begin(now);
    request.begin(now + 1s);
    request.reject(&metrics, RejectionReason::rate_limit);
    request.reject(&metrics, RejectionReason::rate_limit);
    request.fail(&metrics, apigate::ProxyFailureStage::timeout);
    request.fail(&metrics, apigate::ProxyFailureStage::timeout);
    EXPECT_TRUE(request.finish(&metrics, 429, now + 250ms));
    EXPECT_FALSE(request.finish(&metrics, 503, now + 2s));
    request.reject(&metrics, RejectionReason::proxy_capacity);
    request.fail(&metrics, apigate::ProxyFailureStage::read);
    RequestObservation protocol;
    EXPECT_TRUE(protocol.finish(&metrics, 400, now));
    RequestObservation cancelled;
    cancelled.begin(now);  // No final response: no observation, even when destroyed.
    RequestObservation scrape;
    scrape.exclude(true);
    scrape.begin(now);
    scrape.reject(&metrics, RejectionReason::buffer_capacity);
    scrape.fail(&metrics, apigate::ProxyFailureStage::connect);
    EXPECT_TRUE(scrape.finish(&metrics, 200, now + 1s));
    const auto text = render(metrics);
    EXPECT_EQ(Metrics::max_output_bytes, 131072U);
    EXPECT_NE(text.find("# HELP apigate_http_responses_total Final responses generated, "
                        "not delivery acknowledgements; excludes metrics requests.\n"
                        "# TYPE apigate_http_responses_total counter\n"),
              std::string::npos);
    EXPECT_NE(text.find("# TYPE apigate_request_processing_seconds summary\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE apigate_active_connections gauge\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"429\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"400\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"503\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"200\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("reason=\"rate_limit\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("reason=\"proxy_capacity\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("reason=\"buffer_capacity\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("stage=\"timeout\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("stage=\"read\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_request_processing_seconds_sum 0.25\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_request_processing_seconds_count 1\n"), std::string::npos);
}

TEST(MetricsTest, LateStreamingFailureDoesNotDuplicateStatusOrDuration)
{
    Metrics metrics;
    RequestObservation request;
    const RequestObservation::Clock::time_point now{};
    request.begin(now);
    ASSERT_TRUE(request.finish(&metrics, 200, now + 250ms));
    request.fail(&metrics, apigate::ProxyFailureStage::timeout);
    EXPECT_NE(render(metrics).find("stage=\"timeout\"} 0\n"), std::string::npos);
    request.fail_stream(&metrics, apigate::ProxyFailureStage::timeout);
    request.fail_stream(&metrics, apigate::ProxyFailureStage::read);
    EXPECT_FALSE(request.finish(&metrics, 504, now + 1s));
    const auto text = render(metrics);
    EXPECT_NE(text.find("status=\"200\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"504\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("stage=\"timeout\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("stage=\"read\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_request_processing_seconds_sum 0.25\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_request_processing_seconds_count 1\n"), std::string::npos);
}

TEST(MetricsTest, BoundsNumbersAndUsesFixedStatusAndFailureLabels)
{
    Metrics metrics;
    for (const auto status : {0U, 99U, 600U, std::numeric_limits<unsigned int>::max()})
    {
        metrics.response(status);
    }
    metrics.response(100);
    metrics.response(599);
    metrics.processing(std::numeric_limits<double>::max());
    metrics.processing(std::numeric_limits<double>::max());
    metrics.processing(std::numeric_limits<double>::infinity());
    metrics.processing(std::numeric_limits<double>::quiet_NaN());
    metrics.processing(-1);
    auto count = std::numeric_limits<std::uint64_t>::max() - 1;
    apigate::detail::saturating_increment(count);
    apigate::detail::saturating_increment(count);
    EXPECT_EQ(count, std::numeric_limits<std::uint64_t>::max());
    const auto text = render(metrics);
    EXPECT_NE(text.find("status=\"other\"} 4\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"100\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("status=\"599\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_request_processing_seconds_count 2\n"), std::string::npos);
    EXPECT_EQ(text.find("Inf"), std::string::npos);
    EXPECT_EQ(text.find("NaN"), std::string::npos);
}

class DecimalComma : public std::numpunct<char>
{
   protected:
    char do_decimal_point() const override
    {
        return ',';
    }
};

TEST(MetricsTest, MaximumSlotsHaveUniqueBoundedSeriesAndRealSnapshots)
{
    apigate::AppConfig config;
    for (int route = 0; route < 32; ++route)
    {
        config.routes.push_back(
            {"/private-prefix", std::vector<apigate::UpstreamConfig>(16, {"private-host", 1234}),
             false, std::nullopt, apigate::HealthCheckConfig{"/private-probe", 100, 50, 1, 1}});
    }
    apigate::detail::UpstreamHealthState health{config.routes};
    EXPECT_EQ(health.record(31, 15, false), apigate::detail::HealthTransition::unhealthy);
    apigate::detail::AcceptCapacityState connections{256};
    connections.session_started();
    auto proxies = std::make_shared<apigate::detail::ProxyAdmissionState>(32);
    auto permit = apigate::detail::ProxyPermit::try_acquire(proxies);
    apigate::detail::BufferBudgetState budget{67108864};
    auto bytes = budget.try_acquire(65536);
    Metrics metrics;
    metrics.processing(0.25);
    const auto original_locale = std::locale();
    const std::locale comma{original_locale, new DecimalComma};
    // A stream with this locale would print 0,25; metrics uses locale-free to_chars.
    std::ostringstream localized;
    localized.imbue(comma);
    localized << 0.25;
    EXPECT_EQ(localized.str(), "0,25");
    std::string text;
    {
        struct RestoreLocale
        {
            std::locale saved;
            ~RestoreLocale()
            {
                std::locale::global(saved);
            }
        } restore{original_locale};
        std::locale::global(comma);
        text = metrics.render(config, connections, *proxies, budget, health);
    }
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find('\r'), std::string::npos);
    EXPECT_LE(text.size(), Metrics::max_output_bytes);
    EXPECT_NE(text.find("apigate_request_processing_seconds_sum 0.25\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_active_connections 1\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_active_proxies 1\n"), std::string::npos);
    EXPECT_NE(text.find("apigate_in_flight_body_bytes 65536\n"), std::string::npos);
    EXPECT_NE(text.find("route_index=\"31\",upstream_index=\"15\"} 0\n"), std::string::npos);
    EXPECT_NE(text.find("route_index=\"0\",upstream_index=\"0\"} 1\n"), std::string::npos);
    EXPECT_EQ(text.find("private-"), std::string::npos);
    std::istringstream lines{text};
    std::set<std::string> series;
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.front() != '#')
        {
            EXPECT_TRUE(series.insert(line.substr(0, line.find(' '))).second);
        }
    }
    EXPECT_EQ(series.size(), 1033U);  // 501 + 4 + 8 + 2 + 6 + 512
    permit.reset();
    bytes.reset();
    EXPECT_EQ(budget.current_bytes(), 0U);
    EXPECT_EQ(proxies->active_proxies(), 0U);
    EXPECT_EQ(health.record(31, 15, true), apigate::detail::HealthTransition::healthy);
    EXPECT_NE(metrics.render(config, connections, *proxies, budget, health)
                  .find("route_index=\"31\",upstream_index=\"15\"} 1\n"),
              std::string::npos);
    EXPECT_EQ(render(metrics).find("apigate_upstream_available{"), std::string::npos);
    config.routes.push_back(config.routes.front());
    EXPECT_THROW(static_cast<void>(metrics.render(config, connections, *proxies, budget, health)),
                 std::length_error);
}

TEST(MetricsTest, EnabledEndpointIsExactAndProtocolRulesDoNotChangeDisabledRouting)
{
    apigate::AppConfig config;
    config.routes = {{"/", {{"localhost", 1}}}};
    apigate::HttpRequest request{boost::beast::http::verb::get, "/metrics?private", 11};
    EXPECT_TRUE(apigate::select_http_route(config, request));
    config.metrics_enabled = true;
    EXPECT_FALSE(apigate::select_http_route(config, request));
    request.method(boost::beast::http::verb::post);
    auto response = apigate::handle_http_request(config, request);
    EXPECT_EQ(response.result_int(), 405U);
    EXPECT_EQ(response[boost::beast::http::field::allow], "GET");
    request.method(boost::beast::http::verb::get);
    request.body() = "private";
    EXPECT_EQ(apigate::handle_http_request(config, request).result_int(), 400U);
    request.body().clear();
    request.target("/metrics-other");
    EXPECT_TRUE(apigate::select_http_route(config, request));
}
}  // namespace
