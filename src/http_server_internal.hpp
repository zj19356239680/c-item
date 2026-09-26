#ifndef APIGATE_HTTP_SERVER_INTERNAL_HPP_
#define APIGATE_HTTP_SERVER_INTERNAL_HPP_

#include <boost/system/error_code.hpp>
#include <cstdint>

namespace apigate::detail {

enum class HttpWriteDisposition : std::uint8_t {
    success,
    expected_cancellation,
    client_disconnected,
    unexpected_error,
};

[[nodiscard]] HttpWriteDisposition classify_http_write_result(
    const boost::system::error_code& error, bool stopping) noexcept;

}  // namespace apigate::detail

#endif  // APIGATE_HTTP_SERVER_INTERNAL_HPP_
