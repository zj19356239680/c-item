#ifndef APIGATE_HTTP_SERVER_HPP_
#define APIGATE_HTTP_SERVER_HPP_

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>

namespace apigate {

struct AppConfig;
class StructuredLogger;

class HttpServer {
   public:
    HttpServer(boost::asio::io_context& io_context, const AppConfig& config,
               StructuredLogger& logger);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace apigate

#endif  // APIGATE_HTTP_SERVER_HPP_
