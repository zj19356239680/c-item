#ifndef APIGATE_HTTP_PROXY_HPP_
#define APIGATE_HTTP_PROXY_HPP_

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "apigate/config.hpp"
#include "apigate/http_handler.hpp"

namespace apigate {

enum class ProxyFailureStage : std::uint8_t {
    resolve,
    connect,
    write,
    read,
    timeout,
    response_too_large,
    invalid_response,
};

[[nodiscard]] std::string_view to_string(ProxyFailureStage stage) noexcept;

struct ProxyFailure {
    ProxyFailureStage stage;
    int error_code;
};

struct ProxyResult {
    HttpResponse response;
    std::optional<ProxyFailure> failure;
};

namespace detail {

class ProxyCompletionState {
   public:
    [[nodiscard]] bool claim() noexcept;

   private:
    bool completed_{false};
};

template <typename HandlerAction, typename CleanupAction, typename AbortAction>
void run_proxy_handler_action(ProxyCompletionState& state, HandlerAction&& handler,
                              CleanupAction&& cleanup, AbortAction&& abort) noexcept {
    static_assert(std::is_nothrow_invocable_v<CleanupAction&&>);
    static_assert(std::is_nothrow_invocable_v<AbortAction&&>);

    try {
        std::forward<HandlerAction>(handler)();
    } catch (...) {
        if (!state.claim()) {
            return;
        }
        std::forward<CleanupAction>(cleanup)();
        std::forward<AbortAction>(abort)();
    }
}

template <typename CompletionAction, typename AbortAction>
void run_proxy_completion_action(ProxyCompletionState& state, CompletionAction&& complete,
                                 AbortAction&& abort) noexcept {
    static_assert(std::is_nothrow_invocable_v<AbortAction&&>);

    if (!state.claim()) {
        return;
    }
    try {
        std::forward<CompletionAction>(complete)();
    } catch (...) {
        std::forward<AbortAction>(abort)();
    }
}

[[nodiscard]] std::string make_upstream_authority(std::string_view host, std::uint16_t port);
[[nodiscard]] HttpRequest make_upstream_request(const UpstreamConfig& config,
                                                const HttpRequest& request);
[[nodiscard]] HttpResponse make_downstream_response(const HttpRequest& request,
                                                    const HttpResponse& upstream_response);

}  // namespace detail

class HttpProxyExchange : public std::enable_shared_from_this<HttpProxyExchange> {
   public:
    using CompletionCallback = std::function<void(ProxyResult)>;
    using AbortCallback = std::function<void()>;

    HttpProxyExchange(const boost::asio::any_io_executor& executor, const UpstreamConfig& config,
                      const HttpRequest& request, CompletionCallback on_complete,
                      AbortCallback on_abort);
    ~HttpProxyExchange();

    HttpProxyExchange(const HttpProxyExchange&) = delete;
    HttpProxyExchange& operator=(const HttpProxyExchange&) = delete;

    void start();
    void cancel() noexcept;

   private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace apigate

#endif  // APIGATE_HTTP_PROXY_HPP_
