#ifndef APIGATE_HTTP_SESSION_PROBE_HPP_
#define APIGATE_HTTP_SESSION_PROBE_HPP_

#include <boost/asio/ip/tcp.hpp>
#include <cstddef>
#include <functional>
#include <memory>

#include "apigate/config.hpp"
#include "apigate/logging.hpp"
#include "buffer_budget.hpp"
#include "http_server_internal.hpp"

// Defined only in the separate test target's copy of http_server.cpp. Neither
// these controls nor the probe methods are compiled into apigate_core/api-gate.
namespace apigate::detail
{

struct SessionWriteSnapshot
{
    bool pending;
    std::size_t body_bytes;
};

struct SessionWriteProbe
{
    std::function<SessionWriteSnapshot()> snapshot;
    std::function<void()> stop;
};

[[nodiscard]] SessionWriteProbe start_session_write_probe(
    boost::asio::ip::tcp::socket socket, const AppConfig& config, StructuredLogger& logger,
    std::shared_ptr<ProxyAdmissionState> admission, std::shared_ptr<BufferBudgetState> budget,
    std::function<void()> on_close);

}  // namespace apigate::detail

#endif  // APIGATE_HTTP_SESSION_PROBE_HPP_
