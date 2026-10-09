#include "apigate/http_server.hpp"

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "apigate/config.hpp"
#include "apigate/logging.hpp"
#include "http_server_internal.hpp"

namespace {

namespace asio = boost::asio;
using apigate::detail::AcceptCapacityAction;
using apigate::detail::AcceptCapacityState;
using apigate::detail::AcceptRetryDisposition;
using apigate::detail::classify_accept_retry_completion;
using apigate::detail::classify_drain_timer_completion;
using apigate::detail::classify_http_write_result;
using apigate::detail::classify_session_drain;
using apigate::detail::DrainStartResult;
using apigate::detail::DrainState;
using apigate::detail::DrainTimerDisposition;
using apigate::detail::HttpWriteDisposition;
using apigate::detail::ProxyAdmissionState;
using apigate::detail::ProxyPermit;
using apigate::detail::run_accept_retry_failure_actions;
using apigate::detail::RuntimeFailureState;
using apigate::detail::ServerDrainPhase;
using apigate::detail::SessionDrainDisposition;
using apigate::detail::SessionPhase;

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

TEST(DrainStateTest, CompletesImmediatelyWithoutSessionsAndIgnoresRepeatedStart) {
    DrainState state;

    EXPECT_EQ(state.begin_drain(), DrainStartResult::completed);
    EXPECT_EQ(state.phase(), ServerDrainPhase::completed);
    EXPECT_EQ(state.active_sessions(), 0U);
    EXPECT_EQ(state.begin_drain(), DrainStartResult::unchanged);
    EXPECT_EQ(state.begin_force_stop(), DrainStartResult::unchanged);
}

TEST(DrainStateTest, WaitsForEverySessionAndCompletesOnlyOnce) {
    DrainState state;
    state.session_started();
    state.session_started();

    EXPECT_EQ(state.begin_drain(), DrainStartResult::started);
    EXPECT_EQ(state.phase(), ServerDrainPhase::draining);
    EXPECT_FALSE(state.session_finished());
    EXPECT_EQ(state.active_sessions(), 1U);
    EXPECT_TRUE(state.session_finished());
    EXPECT_EQ(state.phase(), ServerDrainPhase::completed);
    EXPECT_FALSE(state.session_finished());
    EXPECT_EQ(state.active_sessions(), 0U);
}

TEST(DrainStateTest, FatalOrDeadlineCanForceAStartedDrain) {
    DrainState deadline_state;
    deadline_state.session_started();
    ASSERT_EQ(deadline_state.begin_drain(), DrainStartResult::started);
    EXPECT_EQ(deadline_state.begin_force_stop(), DrainStartResult::started);
    EXPECT_EQ(deadline_state.phase(), ServerDrainPhase::force_stopping);
    EXPECT_TRUE(deadline_state.session_finished());
    EXPECT_EQ(deadline_state.phase(), ServerDrainPhase::completed);

    DrainState fatal_state;
    fatal_state.session_started();
    EXPECT_EQ(fatal_state.begin_force_stop(), DrainStartResult::started);
    EXPECT_EQ(fatal_state.phase(), ServerDrainPhase::force_stopping);
}

TEST(DrainTimerDispositionTest, DistinguishesTimeoutCancellationAndFatalErrors) {
    const boost::system::error_code operation_aborted = asio::error::operation_aborted;
    const boost::system::error_code timer_error = asio::error::network_down;

    EXPECT_EQ(classify_drain_timer_completion({}, ServerDrainPhase::draining),
              DrainTimerDisposition::timed_out);
    EXPECT_EQ(classify_drain_timer_completion(operation_aborted, ServerDrainPhase::completed),
              DrainTimerDisposition::ignored);
    EXPECT_EQ(classify_drain_timer_completion(timer_error, ServerDrainPhase::force_stopping),
              DrainTimerDisposition::ignored);
    EXPECT_EQ(classify_drain_timer_completion(operation_aborted, ServerDrainPhase::draining),
              DrainTimerDisposition::fatal);
    EXPECT_EQ(classify_drain_timer_completion(timer_error, ServerDrainPhase::draining),
              DrainTimerDisposition::fatal);
}

TEST(SessionDrainDispositionTest, ClosesReadsAndWaitsForDispatchedWork) {
    EXPECT_EQ(classify_session_drain(SessionPhase::reading),
              SessionDrainDisposition::close_immediately);
    EXPECT_EQ(classify_session_drain(SessionPhase::proxying),
              SessionDrainDisposition::wait_for_current);
    EXPECT_EQ(classify_session_drain(SessionPhase::writing),
              SessionDrainDisposition::wait_for_current);
    EXPECT_EQ(classify_session_drain(SessionPhase::finished), SessionDrainDisposition::ignored);
}

TEST(AcceptCapacityStateTest, PausesAndResumesWithoutDuplicateAccepts) {
    AcceptCapacityState state{1};

    EXPECT_EQ(state.next_action(), AcceptCapacityAction::start_accept);
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::none);
    state.accept_completed();
    state.session_started();
    EXPECT_EQ(state.active_connections(), 1U);
    EXPECT_EQ(state.max_connections(), 1U);
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::pause);
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::none);

    state.session_finished();
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::resume_and_start);
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::none);
}

TEST(AcceptCapacityStateTest, CoordinatesRetryAndStopWithCapacity) {
    AcceptCapacityState state{2};

    EXPECT_EQ(state.next_action(), AcceptCapacityAction::start_accept);
    state.accept_completed();
    state.retry_scheduled();
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::none);
    state.retry_completed();
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::start_accept);
    state.stop();
    state.accept_completed();
    EXPECT_EQ(state.next_action(), AcceptCapacityAction::none);
}

TEST(ProxyAdmissionStateTest, EnforcesLimitAndReusesReleasedCapacity) {
    const auto state = std::make_shared<ProxyAdmissionState>(2);
    auto first = ProxyPermit::try_acquire(state);
    auto second = ProxyPermit::try_acquire(state);

    if (!first || !second) {
        FAIL() << "expected both permits to be admitted";
        return;
    }
    EXPECT_EQ(state->active_proxies(), 2U);
    EXPECT_EQ(state->max_proxies(), 2U);
    EXPECT_FALSE(ProxyPermit::try_acquire(state).has_value());

    first->release();
    first->release();
    EXPECT_EQ(state->active_proxies(), 1U);
    EXPECT_TRUE(ProxyPermit::try_acquire(state).has_value());
    EXPECT_EQ(state->active_proxies(), 1U);
}

TEST(ProxyAdmissionStateTest, MoveKeepsASinglePermitOwner) {
    const auto state = std::make_shared<ProxyAdmissionState>(1);
    auto permit = ProxyPermit::try_acquire(state);
    if (!permit) {
        FAIL() << "expected permit to be admitted";
        return;
    }

    ProxyPermit moved{std::move(permit.value())};

    EXPECT_FALSE(permit->owns_capacity());
    EXPECT_TRUE(moved.owns_capacity());
    permit->release();
    EXPECT_EQ(state->active_proxies(), 1U);
    moved.release();
    moved.release();
    EXPECT_EQ(state->active_proxies(), 0U);
}

TEST(ProxyAdmissionStateTest, TerminalPathsReleaseExactlyOnce) {
    enum class TerminalPath : std::uint8_t {
        success,
        failure,
        cancellation,
        exception,
    };
    const TerminalPath paths[] = {TerminalPath::success, TerminalPath::failure,
                                  TerminalPath::cancellation, TerminalPath::exception};

    for (const auto path : paths) {
        const auto state = std::make_shared<ProxyAdmissionState>(1);
        bool exception_observed = false;
        try {
            auto permit = ProxyPermit::try_acquire(state);
            if (!permit) {
                ADD_FAILURE() << "expected permit to be admitted";
                continue;
            }
            if (path == TerminalPath::exception) {
                throw std::runtime_error{"simulated proxy transition failure"};
            }
            permit->release();
            permit->release();
        } catch (const std::runtime_error&) {
            exception_observed = true;
        }
        EXPECT_EQ(exception_observed, path == TerminalPath::exception);
        EXPECT_EQ(state->active_proxies(), 0U);
        EXPECT_TRUE(ProxyPermit::try_acquire(state).has_value());
        EXPECT_EQ(state->active_proxies(), 0U);
    }
}

TEST(HttpServerTest, RejectsEmptyRuntimeFailureCallback) {
    boost::asio::io_context io_context;
    const apigate::AppConfig config;
    apigate::StructuredLogger logger{config};

    EXPECT_THROW(static_cast<void>(apigate::HttpServer{io_context, config, logger,
                                                       std::function<void()>{}, []() noexcept {}}),
                 std::invalid_argument);
}

TEST(HttpServerTest, RejectsEmptyDrainCompletionCallback) {
    boost::asio::io_context io_context;
    const apigate::AppConfig config;
    apigate::StructuredLogger logger{config};

    EXPECT_THROW(static_cast<void>(apigate::HttpServer{io_context, config, logger, []() noexcept {},
                                                       std::function<void()>{}}),
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
