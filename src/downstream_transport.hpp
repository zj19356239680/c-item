#ifndef APIGATE_DOWNSTREAM_TRANSPORT_HPP_
#define APIGATE_DOWNSTREAM_TRANSPORT_HPP_

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "apigate/http_handler.hpp"
#include "downstream_tls.hpp"
#include "stream_response.hpp"

namespace apigate::detail
{
// Narrow transport adapter. Session owns messages, sequencing, deadlines and cancellation.
// Every operation's session-owning callback keeps this transport/context alive until completion.
class DownstreamTransport
{
   public:
    using Completion = std::function<void(const boost::system::error_code&)>;
    using Parser = boost::beast::http::request_parser<boost::beast::http::string_body>;
    DownstreamTransport(boost::asio::ip::tcp::socket socket,
                        std::shared_ptr<DownstreamTlsContext> context);
    ~DownstreamTransport();
    DownstreamTransport(const DownstreamTransport&) = delete;
    DownstreamTransport& operator=(const DownstreamTransport&) = delete;
    [[nodiscard]] bool is_tls() const noexcept;
    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept;
    [[nodiscard]] boost::asio::any_io_executor get_executor() noexcept;
    void expires_after(std::chrono::steady_clock::duration timeout);
    void async_handshake(Completion completion);
    static Parser& reset_parser(std::optional<Parser>& parser);
    void async_read_header(boost::beast::flat_buffer& buffer, Parser& parser,
                           Completion completion);
    void async_read(boost::beast::flat_buffer& buffer, Parser& parser, Completion completion);
    void async_write(HttpResponse& response, Completion completion);
    void async_stream_header(std::shared_ptr<StreamWriter> writer, Completion completion);
    void async_stream_body(std::shared_ptr<StreamWriter> writer, std::shared_ptr<StreamBlock> block,
                           std::size_t bytes, bool final, Completion completion);
    void async_shutdown(Completion completion);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace apigate::detail
#endif  // APIGATE_DOWNSTREAM_TRANSPORT_HPP_
