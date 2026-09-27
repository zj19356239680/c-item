#include "apigate/http_server.hpp"

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string_view>

#include "apigate/config.hpp"
#include "apigate/logging.hpp"
#include "http_server_internal.hpp"

namespace {

namespace asio = boost::asio;
using apigate::detail::AcceptRetryDisposition;
using apigate::detail::classify_accept_retry_completion;
using apigate::detail::classify_http_write_result;
using apigate::detail::HttpWriteDisposition;
using apigate::detail::run_accept_retry_failure_actions;
using apigate::detail::RuntimeFailureState;

TEST(HttpWriteDispositionTest, ClassifiesSuccessfulCompletion) {
    EXPECT_EQ(classify_http_write_result({}, false), HttpWriteDisposition::success);
}

TEST(HttpWriteDispositionTest, TreatsStoppingAsExpectedCancellation) {
    const boost::system::error_code unexpected_error = asio::error::network_down;

    EXPECT_EQ(classify_http_write_result(unexpected_error, true),
              HttpWriteDisposition::expected_cancellation);
}

TEST(HttpWriteDispositionTest, ClassifiesOperationAbortedAsExpectedCancellation) {
    const boost::system::error_code operation_aborted = asio::error::operation_aborted;

    EXPECT_EQ(classify_http_write_result(operation_aborted, false),
              HttpWriteDisposition::expected_cancellation);
}

TEST(HttpWriteDispositionTest, ClassifiesClientDisconnects) {
    const boost::system::error_code client_disconnects[] = {
        asio::error::broken_pipe,
        asio::error::connection_aborted,
        asio::error::connection_reset,
        asio::error::eof,
    };

    for (const auto& error : client_disconnects) {
        EXPECT_EQ(classify_http_write_result(error, false),
                  HttpWriteDisposition::client_disconnected);
    }
}

TEST(HttpWriteDispositionTest, ClassifiesUnexpectedErrors) {
    const boost::system::error_code unexpected_error = asio::error::network_down;

    EXPECT_EQ(classify_http_write_result(unexpected_error, false),
              HttpWriteDisposition::unexpected_error);
}

TEST(AcceptRetryDispositionTest, RetriesAfterSuccessfulCompletion) {
    EXPECT_EQ(classify_accept_retry_completion({}, false), AcceptRetryDisposition::retry);
}

TEST(AcceptRetryDispositionTest, IgnoresCompletionWhileStopping) {
    const boost::system::error_code timer_error = asio::error::network_down;

    EXPECT_EQ(classify_accept_retry_completion({}, true), AcceptRetryDisposition::stopped);
    EXPECT_EQ(classify_accept_retry_completion(timer_error, true), AcceptRetryDisposition::stopped);
}

TEST(AcceptRetryDispositionTest, TreatsTimerErrorAsFatal) {
    const boost::system::error_code timer_error = asio::error::network_down;

    EXPECT_EQ(classify_accept_retry_completion(timer_error, false), AcceptRetryDisposition::fatal);
}

TEST(AcceptRetryDispositionTest, TreatsUnexpectedCancellationAsFatal) {
    const boost::system::error_code operation_aborted = asio::error::operation_aborted;

    EXPECT_EQ(classify_accept_retry_completion(operation_aborted, false),
              AcceptRetryDisposition::fatal);
}

TEST(HttpServerTest, RejectsEmptyRuntimeFailureCallback) {
    boost::asio::io_context io_context;
    const apigate::AppConfig config;
    apigate::StructuredLogger logger{config};

    EXPECT_THROW(
        static_cast<void>(apigate::HttpServer{io_context, config, logger, std::function<void()>{}}),
        std::invalid_argument);
}

TEST(AcceptRetryFailureActionsTest, RunsReportStopAndNotificationOnce) {
    RuntimeFailureState state;
    std::array<std::string_view, 3> actions;
    std::size_t action_count = 0;
    const auto record = [&actions, &action_count](std::string_view action) noexcept {
        if (action_count < actions.size()) {
            actions[action_count] = action;
        }
        ++action_count;
    };

    const auto run_actions = [&state, &record] {
        run_accept_retry_failure_actions(
            state, [&record] { record("report"); }, [&record]() noexcept { record("fallback"); },
            [&record]() noexcept { record("stop"); }, [&record]() noexcept { record("notify"); });
    };

    run_actions();
    run_actions();

    EXPECT_EQ(action_count, 3);
    EXPECT_EQ(actions, (std::array<std::string_view, 3>{"report", "stop", "notify"}));
}

TEST(AcceptRetryFailureActionsTest, StopsAndNotifiesOnceWhenReportThrows) {
    RuntimeFailureState state;
    std::array<std::string_view, 4> actions;
    std::size_t action_count = 0;
    const auto record = [&actions, &action_count](std::string_view action) noexcept {
        if (action_count < actions.size()) {
            actions[action_count] = action;
        }
        ++action_count;
    };

    const auto run_actions = [&state, &record] {
        run_accept_retry_failure_actions(
            state,
            [&record] {
                record("report");
                throw std::runtime_error{"report failed"};
            },
            [&record]() noexcept { record("fallback"); }, [&record]() noexcept { record("stop"); },
            [&record]() noexcept { record("notify"); });
    };

    run_actions();
    run_actions();

    EXPECT_EQ(action_count, 4);
    EXPECT_EQ(actions, (std::array<std::string_view, 4>{"report", "fallback", "stop", "notify"}));
}

}  // namespace
