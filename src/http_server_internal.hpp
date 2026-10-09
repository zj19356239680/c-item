#ifndef APIGATE_HTTP_SERVER_INTERNAL_HPP_
#define APIGATE_HTTP_SERVER_INTERNAL_HPP_

#include <boost/system/error_code.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace apigate::detail {

enum class HttpWriteDisposition : std::uint8_t {
    success,
    expected_cancellation,
    client_disconnected,
    unexpected_error,
};

enum class AcceptRetryDisposition : std::uint8_t {
    retry,
    stopped,
    fatal,
};

enum class AcceptCapacityAction : std::uint8_t {
    none,
    start_accept,
    pause,
    resume_and_start,
};

enum class ServerDrainPhase : std::uint8_t {
    running,
    draining,
    force_stopping,
    completed,
};

enum class DrainStartResult : std::uint8_t {
    unchanged,
    started,
    completed,
};

enum class DrainTimerDisposition : std::uint8_t {
    ignored,
    timed_out,
    fatal,
};

enum class SessionPhase : std::uint8_t {
    reading,
    proxying,
    writing,
    finished,
};

enum class SessionDrainDisposition : std::uint8_t {
    close_immediately,
    wait_for_current,
    ignored,
};

class DrainState {
   public:
    void session_started() noexcept;
    [[nodiscard]] bool session_finished() noexcept;
    [[nodiscard]] DrainStartResult begin_drain() noexcept;
    [[nodiscard]] DrainStartResult begin_force_stop() noexcept;

    [[nodiscard]] ServerDrainPhase phase() const noexcept;
    [[nodiscard]] std::size_t active_sessions() const noexcept;

   private:
    [[nodiscard]] DrainStartResult transition_to(ServerDrainPhase phase) noexcept;

    ServerDrainPhase phase_{ServerDrainPhase::running};
    std::size_t active_sessions_{0};
};

class AcceptCapacityState {
   public:
    explicit AcceptCapacityState(std::size_t max_connections) noexcept;

    [[nodiscard]] AcceptCapacityAction next_action() noexcept;
    void accept_completed() noexcept;
    void retry_scheduled() noexcept;
    void retry_completed() noexcept;
    void session_started() noexcept;
    void session_finished() noexcept;
    void stop() noexcept;

    [[nodiscard]] std::size_t active_connections() const noexcept;
    [[nodiscard]] std::size_t max_connections() const noexcept;

   private:
    std::size_t max_connections_;
    std::size_t active_connections_{0};
    bool accept_pending_{false};
    bool retry_pending_{false};
    bool paused_{false};
    bool stopped_{false};
};

class ProxyAdmissionState;

class ProxyPermit {
   public:
    [[nodiscard]] static std::optional<ProxyPermit> try_acquire(
        std::shared_ptr<ProxyAdmissionState> state) noexcept;
    ~ProxyPermit();

    ProxyPermit(const ProxyPermit&) = delete;
    ProxyPermit& operator=(const ProxyPermit&) = delete;
    ProxyPermit(ProxyPermit&& other) noexcept;
    ProxyPermit& operator=(ProxyPermit&& other) noexcept;

    void release() noexcept;
    [[nodiscard]] bool owns_capacity() const noexcept;

   private:
    friend class ProxyAdmissionState;
    explicit ProxyPermit(std::shared_ptr<ProxyAdmissionState> state) noexcept;

    std::shared_ptr<ProxyAdmissionState> state_;
};

class ProxyAdmissionState {
   public:
    explicit ProxyAdmissionState(std::size_t max_proxies) noexcept;

    [[nodiscard]] std::size_t active_proxies() const noexcept;
    [[nodiscard]] std::size_t max_proxies() const noexcept;

   private:
    friend class ProxyPermit;
    void release() noexcept;

    std::size_t max_proxies_;
    std::size_t active_proxies_{0};
};

class RuntimeFailureState {
   public:
    [[nodiscard]] bool claim() noexcept {
        if (claimed_) {
            return false;
        }
        claimed_ = true;
        return true;
    }

   private:
    bool claimed_{false};
};

template <typename ReportAction, typename FallbackAction, typename StopAction,
          typename NotifyAction>
void run_accept_retry_failure_actions(RuntimeFailureState& state, ReportAction&& report,
                                      FallbackAction&& fallback, StopAction&& stop,
                                      NotifyAction&& notify) noexcept {
    static_assert(std::is_nothrow_invocable_v<FallbackAction&&>);
    static_assert(std::is_nothrow_invocable_v<StopAction&&>);
    static_assert(std::is_nothrow_invocable_v<NotifyAction&&>);

    if (!state.claim()) {
        return;
    }

    try {
        std::forward<ReportAction>(report)();
    } catch (...) {
        std::forward<FallbackAction>(fallback)();
    }
    std::forward<StopAction>(stop)();
    std::forward<NotifyAction>(notify)();
}

[[nodiscard]] HttpWriteDisposition classify_http_write_result(
    const boost::system::error_code& error, bool stopping) noexcept;
[[nodiscard]] AcceptRetryDisposition classify_accept_retry_completion(
    const boost::system::error_code& error, bool stopping) noexcept;
[[nodiscard]] DrainTimerDisposition classify_drain_timer_completion(
    const boost::system::error_code& error, ServerDrainPhase phase) noexcept;
[[nodiscard]] SessionDrainDisposition classify_session_drain(SessionPhase phase) noexcept;

}  // namespace apigate::detail

#endif  // APIGATE_HTTP_SERVER_INTERNAL_HPP_
