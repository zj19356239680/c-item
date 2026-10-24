#ifndef APIGATE_DOWNSTREAM_TLS_HPP_
#define APIGATE_DOWNSTREAM_TLS_HPP_

#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <optional>
#include <string>

namespace apigate
{
// Separate server role: never shared with upstream client verification/trust.
class DownstreamTlsContext
{
   public:
    boost::asio::ssl::context context{boost::asio::ssl::context::tls_server};
};

[[nodiscard]] std::shared_ptr<DownstreamTlsContext> make_downstream_tls_context(
    const std::optional<std::string>& certificate, const std::optional<std::string>& private_key);
}  // namespace apigate
#endif  // APIGATE_DOWNSTREAM_TLS_HPP_
