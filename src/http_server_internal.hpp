#ifndef APIGATE_HTTP_SERVER_INTERNAL_HPP_
#define APIGATE_HTTP_SERVER_INTERNAL_HPP_

#include <boost/system/error_code.hpp>
#include <cstdint>
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

}  // namespace apigate::detail

#endif  // APIGATE_HTTP_SERVER_INTERNAL_HPP_
