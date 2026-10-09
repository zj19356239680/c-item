#include "http_proxy.hpp"

#include <gtest/gtest.h>

#include <boost/beast/http.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

namespace http = boost::beast::http;

[[nodiscard]] apigate::HttpRequest make_request() {
    apigate::HttpRequest request{http::verb::get, "/resource?key=value", 11};
    request.set(http::field::host, "gateway.local");
    request.set(http::field::authorization, "Bearer private-value");
    request.set(http::field::cookie, "session=private-value");
    request.set(http::field::connection, "X-Remove, keep-alive");
    request.set("X-Remove", "hop-value");
    request.set("X-End-To-End", "safe-value");
    request.set("X-Forwarded-For", "untrusted-client-value");
    request.set(http::field::content_length, "0");
    return request;
}

TEST(HttpProxyTest, BuildsAuthoritiesForDnsIpv4AndIpv6) {
    EXPECT_EQ(apigate::detail::make_upstream_authority("upstream.internal", 8080),
              "upstream.internal:8080");
    EXPECT_EQ(apigate::detail::make_upstream_authority("127.0.0.1", 80), "127.0.0.1:80");
    EXPECT_EQ(apigate::detail::make_upstream_authority("::1", 8080), "[::1]:8080");
}

TEST(HttpProxyTest, FiltersRequestHopByHopHeadersAndPreservesEndToEndHeaders) {
    const apigate::UpstreamConfig config{"::1", 8080, 3000};

    const auto upstream = apigate::detail::make_upstream_request(config, make_request());

    EXPECT_EQ(upstream.target(), "/resource?key=value");
    EXPECT_EQ(upstream[http::field::host], "[::1]:8080");
    EXPECT_EQ(upstream[http::field::connection], "close");
    EXPECT_EQ(upstream.find("X-Remove"), upstream.end());
    EXPECT_EQ(upstream.find(http::field::content_length), upstream.end());
    EXPECT_EQ(upstream[http::field::authorization], "Bearer private-value");
    EXPECT_EQ(upstream[http::field::cookie], "session=private-value");
    EXPECT_EQ(upstream["X-End-To-End"], "safe-value");
    EXPECT_EQ(upstream.find("X-Forwarded-For"), upstream.end());
    EXPECT_TRUE(upstream.body().empty());
}

TEST(HttpProxyTest, PreservesBodyMethodAndPayloadWithRegeneratedLength) {
    const apigate::UpstreamConfig config{"upstream.internal", 8080, 3000};
    for (const auto method : {http::verb::post, http::verb::put, http::verb::patch}) {
        auto request = make_request();
        request.method(method);
        request.set(http::field::content_type, "application/octet-stream");
        request.set(http::field::transfer_encoding, "chunked");
        request.body() = "forwarded-payload";

        const auto upstream = apigate::detail::make_upstream_request(config, request);

        EXPECT_EQ(upstream.method(), method);
        EXPECT_EQ(upstream.target(), request.target());
        EXPECT_EQ(upstream.body(), request.body());
        EXPECT_EQ(upstream[http::field::content_type], "application/octet-stream");
        EXPECT_EQ(upstream[http::field::content_length], std::to_string(request.body().size()));
        EXPECT_EQ(upstream.find(http::field::transfer_encoding), upstream.end());
    }
}

TEST(HttpProxyTest, RegeneratesZeroLengthForEmptyBodyMethods) {
    const apigate::UpstreamConfig config{"upstream.internal", 8080, 3000};
    for (const auto method : {http::verb::post, http::verb::put, http::verb::patch}) {
        auto request = make_request();
        request.method(method);

        const auto upstream = apigate::detail::make_upstream_request(config, request);

        EXPECT_EQ(upstream.method(), method);
        EXPECT_TRUE(upstream.body().empty());
        EXPECT_EQ(upstream[http::field::content_length], "0");
    }
}

TEST(HttpProxyTest, FiltersResponseHopByHopHeadersAndOverridesServer) {
    const auto request = make_request();
    apigate::HttpResponse upstream{http::status::created, 11};
    upstream.set(http::field::server, "private-upstream");
    upstream.set(http::field::connection, "X-Remove, close");
    upstream.set(http::field::keep_alive, "timeout=5");
    upstream.set("X-Remove", "hop-value");
    upstream.set("X-End-To-End", "safe-value");
    upstream.body() = "response-body";
    upstream.prepare_payload();

    const auto response = apigate::detail::make_downstream_response(request, upstream);

    EXPECT_EQ(response.result(), http::status::created);
    EXPECT_EQ(response.body(), "response-body");
    EXPECT_EQ(response[http::field::server], "ApiGate");
    EXPECT_EQ(response.find("X-Remove"), response.end());
    EXPECT_EQ(response.find(http::field::keep_alive), response.end());
    EXPECT_EQ(response["X-End-To-End"], "safe-value");
    EXPECT_TRUE(response.keep_alive());
}

TEST(HttpProxyTest, CompletionAndCancellationCanOnlyClaimOnce) {
    apigate::detail::ProxyCompletionState completed;
    EXPECT_TRUE(completed.claim());
    EXPECT_FALSE(completed.claim());

    apigate::detail::ProxyCompletionState cancelled;
    EXPECT_TRUE(cancelled.claim());
    EXPECT_FALSE(cancelled.claim());
}

TEST(HttpProxyTest, ExposesStableFailureStages) {
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::resolve), "resolve");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::connect), "connect");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::write), "write");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::read), "read");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::timeout), "timeout");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::response_too_large),
              "response_too_large");
    EXPECT_EQ(apigate::to_string(apigate::ProxyFailureStage::invalid_response), "invalid_response");
}

TEST(HttpProxyTest, HandlerExceptionCleansUpAndAbortsOnlyOnce) {
    apigate::detail::ProxyCompletionState state;
    std::size_t cleanup_count = 0;
    std::size_t abort_count = 0;
    const auto run_throwing_handler = [&] {
        apigate::detail::run_proxy_handler_action(
            state, [] { throw std::runtime_error{"stage transition failed"}; },
            [&cleanup_count]() noexcept { ++cleanup_count; },
            [&abort_count]() noexcept { ++abort_count; });
    };

    EXPECT_NO_THROW(run_throwing_handler());
    EXPECT_NO_THROW(run_throwing_handler());
    EXPECT_EQ(cleanup_count, 1U);
    EXPECT_EQ(abort_count, 1U);
}

TEST(HttpProxyTest, SuccessfulHandlerDoesNotCleanUpOrAbort) {
    apigate::detail::ProxyCompletionState state;
    std::size_t handler_count = 0;
    std::size_t cleanup_count = 0;
    std::size_t abort_count = 0;

    apigate::detail::run_proxy_handler_action(
        state, [&handler_count] { ++handler_count; },
        [&cleanup_count]() noexcept { ++cleanup_count; },
        [&abort_count]() noexcept { ++abort_count; });

    EXPECT_EQ(handler_count, 1U);
    EXPECT_EQ(cleanup_count, 0U);
    EXPECT_EQ(abort_count, 0U);
}

TEST(HttpProxyTest, CompletionExceptionAbortsOnlyOnce) {
    apigate::detail::ProxyCompletionState state;
    std::size_t completion_count = 0;
    std::size_t abort_count = 0;
    const auto run_throwing_completion = [&] {
        apigate::detail::run_proxy_completion_action(
            state,
            [&completion_count] {
                ++completion_count;
                throw std::runtime_error{"completion failed"};
            },
            [&abort_count]() noexcept { ++abort_count; });
    };

    EXPECT_NO_THROW(run_throwing_completion());
    EXPECT_NO_THROW(run_throwing_completion());
    EXPECT_EQ(completion_count, 1U);
    EXPECT_EQ(abort_count, 1U);
}

}  // namespace
