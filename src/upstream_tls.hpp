#ifndef APIGATE_UPSTREAM_TLS_HPP_
#define APIGATE_UPSTREAM_TLS_HPP_

#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <optional>
#include <string>

#include "apigate/config.hpp"

namespace apigate
{

// Configured once before listening; exchanges retain shared ownership through all TLS callbacks.
class UpstreamTlsContext
{
   public:
    boost::asio::ssl::context context{boost::asio::ssl::context::tls_client};
};

[[nodiscard]] std::shared_ptr<UpstreamTlsContext> make_upstream_tls_context(
    const std::optional<std::string>& ca_file, bool https_enabled);

}  // namespace apigate

#endif  // APIGATE_UPSTREAM_TLS_HPP_
