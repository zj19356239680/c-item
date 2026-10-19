#include "upstream_tls.hpp"

#include <openssl/ssl.h>

#include <array>
#include <boost/system/error_code.hpp>
#include <filesystem>

namespace apigate
{

std::shared_ptr<UpstreamTlsContext> make_upstream_tls_context(
    const std::optional<std::string>& ca_file, bool https_enabled)
{
    if (!ca_file && !https_enabled)
    {
        return nullptr;
    }
    try
    {
        auto trust = std::make_shared<UpstreamTlsContext>();
        auto& context = trust->context;
        if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1)
        {
            throw ConfigError("upstream TLS policy initialization failed");
        }
        const std::array<unsigned char, 9> http11{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        if (SSL_CTX_set_alpn_protos(context.native_handle(), http11.data(),
                                    static_cast<unsigned int>(http11.size())) != 0)
        {
            throw ConfigError("upstream TLS policy initialization failed");
        }
        SSL_CTX_set_session_cache_mode(context.native_handle(), SSL_SESS_CACHE_OFF);
        context.set_verify_mode(boost::asio::ssl::verify_peer);
        if (ca_file)
        {
            // A new context has an empty store. Do not also add system/default trust.
            context.load_verify_file(*ca_file);
        }
        else
        {
            // Use runtime OS bundles, not a build-machine/vcpkg installation path.
            constexpr std::array bundles{"/etc/ssl/certs/ca-certificates.crt",
                                         "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"};
            bool loaded = false;
            for (const auto* bundle : bundles)
            {
                if (std::filesystem::is_regular_file(bundle))
                {
                    context.load_verify_file(bundle);
                    loaded = true;
                    break;
                }
            }
            if (!loaded)
            {
                throw ConfigError("system upstream CA bundle is unavailable");
            }
        }
        return trust;
    }
    catch (...)
    {
        // OpenSSL/filesystem exceptions may contain the configured path. Never forward them.
        throw ConfigError("upstream TLS trust configuration could not be loaded");
    }
}

}  // namespace apigate
