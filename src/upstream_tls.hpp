#ifndef APIGATE_UPSTREAM_TLS_HPP_
#define APIGATE_UPSTREAM_TLS_HPP_

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
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

// Shared by business exchanges and probes; never derive identity from downstream headers.
template <typename TlsStream>
[[nodiscard]] bool configure_upstream_tls_identity(TlsStream& stream, const std::string& host)
{
    stream.set_verify_callback(boost::asio::ssl::host_name_verification(host));
    boost::system::error_code address_error;
    static_cast<void>(boost::asio::ip::make_address(host, address_error));
    return !address_error || SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 1;
}

}  // namespace apigate

#endif  // APIGATE_UPSTREAM_TLS_HPP_
