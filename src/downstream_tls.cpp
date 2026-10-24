#include "downstream_tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstring>

#include "apigate/config.hpp"

namespace apigate
{
namespace
{
int select_http11(SSL*, const unsigned char** output, unsigned char* output_length,
                  const unsigned char* input, unsigned int input_length, void*) noexcept
{
    unsigned int offset = 0;
    while (offset < input_length)
    {
        const auto length = input[offset++];
        if (length == 0 || length > input_length - offset)
        {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (length == 8 && std::memcmp(input + offset, "http/1.1", 8) == 0)
        {
            *output = input + offset;
            *output_length = length;
            return SSL_TLSEXT_ERR_OK;
        }
        offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
}  // namespace

std::shared_ptr<DownstreamTlsContext> make_downstream_tls_context(
    const std::optional<std::string>& certificate, const std::optional<std::string>& private_key)
{
    if (!certificate && !private_key)
    {
        return nullptr;
    }
    if (!certificate || !private_key)
    {
        throw ConfigError("APIGATE_TLS_CERT_FILE and APIGATE_TLS_KEY_FILE must be set together");
    }
    const auto& cert = *certificate;
    const auto& key = *private_key;
    if (cert.empty() || key.empty())
    {
        throw ConfigError("downstream TLS certificate and key must not be empty");
    }
    try
    {
        auto server = std::make_shared<DownstreamTlsContext>();
        auto& context = server->context;
        auto* native = context.native_handle();
        if (SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_max_early_data(native, 0) != 1)
        {
            throw ConfigError("downstream TLS policy initialization failed");
        }
        SSL_CTX_set_options(native, SSL_OP_NO_RENEGOTIATION);
        SSL_CTX_set_alpn_select_cb(native, select_http11, nullptr);
        context.set_verify_mode(boost::asio::ssl::verify_none);  // No client authentication/mTLS.
        // Asio owns password callback userdata and its destruction; use its API rather
        // than replacing that storage through the OpenSSL native handle.
        const auto password_requested = std::make_shared<bool>(false);
        context.set_password_callback(
            [password_requested](std::size_t, boost::asio::ssl::context::password_purpose)
            {
                *password_requested = true;
                return std::string{};  // No interactive/default OpenSSL password prompt.
            });
        context.use_certificate_chain_file(cert);
        context.use_private_key_file(key, boost::asio::ssl::context::pem);
        if (*password_requested || SSL_CTX_check_private_key(native) != 1)
        {
            throw ConfigError("downstream TLS key is encrypted or does not match");
        }
        return server;
    }
    catch (...)
    {
        ERR_clear_error();
        throw ConfigError(
            "downstream TLS certificate and unencrypted matching key could not be loaded");
    }
}
}  // namespace apigate
