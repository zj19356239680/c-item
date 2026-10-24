#include "downstream_transport.hpp"

#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <utility>

namespace apigate::detail
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

class DownstreamTransport::Impl
{
   public:
    Impl(asio::ip::tcp::socket socket, std::shared_ptr<DownstreamTlsContext> context)
        : context_(std::move(context)), plain_(std::move(socket))
    {
        if (context_)
        {
            tls_ = std::make_unique<TlsStream>(std::move(plain_), context_->context);
        }
    }
    using TlsStream = beast::ssl_stream<beast::tcp_stream>;
    beast::tcp_stream& transport() noexcept
    {
        return tls_ ? beast::get_lowest_layer(*tls_) : plain_;
    }
    template <typename Action>
    void with_stream(Action&& action)
    {
        if (tls_)
        {
            std::forward<Action>(action)(*tls_);
        }
        else
        {
            std::forward<Action>(action)(plain_);
        }
    }
    std::shared_ptr<DownstreamTlsContext> context_;
    beast::tcp_stream plain_;
    std::unique_ptr<TlsStream> tls_;
};
DownstreamTransport::DownstreamTransport(asio::ip::tcp::socket socket,
                                         std::shared_ptr<DownstreamTlsContext> context)
    : impl_(std::make_unique<Impl>(std::move(socket), std::move(context)))
{
}
DownstreamTransport::~DownstreamTransport() = default;
bool DownstreamTransport::is_tls() const noexcept
{
    return impl_->tls_ != nullptr;
}
asio::ip::tcp::socket& DownstreamTransport::socket() noexcept
{
    return impl_->transport().socket();
}
asio::any_io_executor DownstreamTransport::get_executor() noexcept
{
    return impl_->transport().get_executor();
}
void DownstreamTransport::expires_after(std::chrono::steady_clock::duration timeout)
{
    impl_->transport().expires_after(timeout);
}
void DownstreamTransport::async_handshake(Completion completion)
{
    impl_->tls_->async_handshake(asio::ssl::stream_base::server, std::move(completion));
}
DownstreamTransport::Parser& DownstreamTransport::reset_parser(std::optional<Parser>& parser)
{
    // Construct and consume this polymorphic Beast template in the same translation unit,
    // ensuring its virtual parsing members are instantiated where reads are compiled.
    return parser.emplace();
}
void DownstreamTransport::async_read_header(beast::flat_buffer& buffer, Parser& parser,
                                            Completion completion)
{
    impl_->with_stream(
        [&](auto& stream)
        {
            http::async_read_header(stream, buffer, parser,
                                    [completion = std::move(completion)](
                                        const boost::system::error_code& error, std::size_t)
                                    {
                                        completion(error);
                                    });
        });
}
void DownstreamTransport::async_read(beast::flat_buffer& buffer, Parser& parser,
                                     Completion completion)
{
    impl_->with_stream(
        [&](auto& stream)
        {
            http::async_read(stream, buffer, parser,
                             [completion = std::move(completion)](
                                 const boost::system::error_code& error, std::size_t)
                             {
                                 completion(error);
                             });
        });
}
void DownstreamTransport::async_write(HttpResponse& response, Completion completion)
{
    impl_->with_stream(
        [&](auto& stream)
        {
            http::async_write(stream, response,
                              [completion = std::move(completion)](
                                  const boost::system::error_code& error, std::size_t)
                              {
                                  completion(error);
                              });
        });
}
void DownstreamTransport::async_shutdown(Completion completion)
{
    impl_->tls_->async_shutdown(std::move(completion));
}
}  // namespace apigate::detail
