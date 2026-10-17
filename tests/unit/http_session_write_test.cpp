#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "apigate/http_handler.hpp"
#include "http_session_probe.hpp"

namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
constexpr std::size_t response_bytes = 1024 * 1024;

struct Client {
    explicit Client(asio::io_context& context) : socket(context) {
        parser.body_limit(response_bytes);
    }
    tcp::socket socket;
    boost::beast::flat_buffer buffer;
    apigate::HttpRequest request;
    http::response_parser<http::string_body> parser;
    bool header_done{false};
    bool done{false};
    boost::system::error_code error;
};

struct UpstreamPeer {
    explicit UpstreamPeer(tcp::socket accepted) : socket(std::move(accepted)) {}
    tcp::socket socket;
    boost::beast::flat_buffer buffer;
    apigate::HttpRequest request;
    apigate::HttpResponse response;
};

class SessionWriteRegressionTest : public testing::Test {
   protected:
    void SetUp() override {
        testing::internal::CaptureStdout();
        testing::internal::CaptureStderr();
        config_.max_in_flight_bytes = response_bytes;
        config_.upstream =
            apigate::UpstreamConfig{"127.0.0.1", upstream_acceptor_.local_endpoint().port(), 5000};
        logger_ = std::make_unique<apigate::StructuredLogger>(config_);
        accept_upstream();
    }

    void TearDown() override {
        for (auto& probe : probes_) {
            probe.stop();
            probe.stop();
        }
        probes_.clear();
        close_socket(upstream_acceptor_);
        close_socket(downstream_acceptor_);
        for (auto& client : clients_) {
            close_socket(client->socket);
        }
        for (auto& peer : upstream_peers_) {
            close_socket(peer->socket);
        }
        context_.restart();
        context_.run_for(1s);
        EXPECT_EQ(budget_->current_bytes(), 0U);
        EXPECT_EQ(admission_->active_proxies(), 0U);
        for (const auto count : close_counts_) {
            EXPECT_EQ(count, 1U);
        }
        logger_.reset();
        const auto logs = testing::internal::GetCapturedStdout();
        EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
        EXPECT_EQ(logs.find("http_response_write_failed"), std::string::npos);
        EXPECT_EQ(logs.find("http_upstream_request_failed"), std::string::npos);
        EXPECT_EQ(logs.find("critical"), std::string::npos);
        EXPECT_EQ(logs.find("http_proxy_rejected_capacity"), std::string::npos);
        EXPECT_EQ(logs.find("write-body-marker"), std::string::npos);
    }

    template <typename Socket>
    static void close_socket(Socket& socket) {
        boost::system::error_code error;
        socket.close(error);
        EXPECT_FALSE(error);
    }

    template <typename Predicate>
    bool run_until(Predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        context_.restart();
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                context_.run_one_until(deadline) == 0) {
                return ready();
            }
        }
        return true;
    }

    void accept_upstream() {
        upstream_acceptor_.async_accept([this](const boost::system::error_code& error,
                                               tcp::socket socket) {
            if (error) {
                return;
            }
            auto peer = std::make_shared<UpstreamPeer>(std::move(socket));
            upstream_peers_.push_back(peer);
            accept_upstream();
            http::async_read(
                peer->socket, peer->buffer, peer->request,
                [this, peer](const boost::system::error_code& read_error, std::size_t) {
                    if (read_error) {
                        return;
                    }
                    peer->response = apigate::HttpResponse{http::status::ok, 11};
                    peer->response.body() = expected_body_;
                    peer->response.keep_alive(false);
                    peer->response.prepare_payload();
                    http::async_write(
                        peer->socket, peer->response,
                        [this, peer](const boost::system::error_code& write_error, std::size_t) {
                            if (!write_error) {
                                ++upstream_writes_completed_;
                            }
                            close_socket(peer->socket);
                        });
                });
        });
    }

    Client& start_client(bool header_only, bool request_body = false) {
        const auto index = clients_.size();
        auto client = std::make_unique<Client>(context_);
        client->socket.open(tcp::v4());
        // Set before connect so the advertised receive window is also bounded.
        client->socket.set_option(tcp::socket::receive_buffer_size(4096));
        tcp::socket::receive_buffer_size receive_size;
        client->socket.get_option(receive_size);
        EXPECT_LT(receive_size.value(), static_cast<int>(response_bytes / 8));
        client->request =
            apigate::HttpRequest{request_body ? http::verb::post : http::verb::get, "/write", 11};
        client->request.set(http::field::host, "localhost");
        client->request.keep_alive(false);
        if (request_body) {
            client->request.body() = "x";
        }
        client->request.prepare_payload();
        clients_.push_back(std::move(client));
        close_counts_.push_back(0);
        downstream_acceptor_.async_accept(
            [this, index](const boost::system::error_code& error, tcp::socket socket) {
                if (error) {
                    return;
                }
                socket.set_option(tcp::socket::send_buffer_size(4096));
                tcp::socket::send_buffer_size send_size;
                socket.get_option(send_size);
                EXPECT_LT(send_size.value(), static_cast<int>(response_bytes / 8));
                probes_.push_back(apigate::detail::start_session_write_probe(
                    std::move(socket), config_, *logger_, admission_, budget_,
                    [this, index] { ++close_counts_[index]; }));
            });
        auto& result = *clients_.back();
        result.socket.async_connect(
            downstream_acceptor_.local_endpoint(),
            [this, index, header_only](const boost::system::error_code& error) {
                auto& current = *clients_[index];
                if (error) {
                    current.error = error;
                    current.done = true;
                    return;
                }
                http::async_write(
                    current.socket, current.request,
                    [this, index, header_only](const boost::system::error_code& write_error,
                                               std::size_t) {
                        auto& writing = *clients_[index];
                        if (write_error) {
                            writing.error = write_error;
                            writing.done = true;
                            return;
                        }
                        if (header_only) {
                            http::async_read_header(
                                writing.socket, writing.buffer, writing.parser,
                                [this, index](const boost::system::error_code& read_error,
                                              std::size_t) {
                                    clients_[index]->error = read_error;
                                    clients_[index]->header_done = true;
                                });
                        } else {
                            read_body(index);
                        }
                    });
            });
        return result;
    }

    void read_body(std::size_t index) {
        auto& client = *clients_[index];
        http::async_read(client.socket, client.buffer, client.parser,
                         [this, index](const boost::system::error_code& error, std::size_t) {
                             clients_[index]->error = error;
                             clients_[index]->done = true;
                         });
    }

    void assert_blocked_write() {
        // This observes the actual session after proxy completion, not merely
        // the client's header. Poll all ready handlers without reading the body.
        context_.poll();
        ASSERT_FALSE(probes_.empty());
        const auto snapshot = probes_.front().snapshot();
        ASSERT_TRUE(snapshot.pending);
        ASSERT_EQ(snapshot.body_bytes, response_bytes);
        ASSERT_EQ(budget_->current_bytes(), response_bytes);
        ASSERT_EQ(admission_->active_proxies(), 0U);
        ASSERT_EQ(upstream_writes_completed_, 1U);
    }

    void assert_budget_rejection() {
        auto& competitor = start_client(false, true);
        ASSERT_TRUE(run_until([&competitor] { return competitor.done; }));
        ASSERT_FALSE(competitor.error);
        EXPECT_EQ(competitor.parser.get().result(), http::status::service_unavailable);
        const auto error = nlohmann::json::parse(competitor.parser.get().body());
        EXPECT_EQ(error["error"]["code"], "gateway_overloaded");
        EXPECT_EQ(error["error"]["message"], "buffer capacity is exhausted");
        EXPECT_EQ(budget_->current_bytes(), response_bytes);
        EXPECT_EQ(upstream_peers_.size(), 1U);
    }

    void assert_full_recovery() {
        ASSERT_TRUE(run_until([this] { return budget_->current_bytes() == 0; }));
        auto& recovered = start_client(false);
        ASSERT_TRUE(run_until([&recovered] { return recovered.done; }));
        ASSERT_FALSE(recovered.error);
        EXPECT_EQ(recovered.parser.get().result(), http::status::ok);
        EXPECT_TRUE(recovered.parser.get().body() == expected_body_);
        ASSERT_TRUE(run_until([this] { return budget_->current_bytes() == 0; }));
        auto full = budget_->try_acquire(response_bytes);
        ASSERT_TRUE(full.has_value());
        for (auto& probe : probes_) {
            probe.stop();
            probe.stop();
        }
        context_.poll();
        EXPECT_EQ(budget_->current_bytes(), response_bytes);
        EXPECT_FALSE(budget_->try_acquire(1).has_value());
        full.reset();
        EXPECT_EQ(budget_->current_bytes(), 0U);
    }

    asio::io_context context_;
    tcp::acceptor upstream_acceptor_{context_, {asio::ip::make_address("127.0.0.1"), 0}};
    tcp::acceptor downstream_acceptor_{context_, {asio::ip::make_address("127.0.0.1"), 0}};
    apigate::AppConfig config_;
    std::unique_ptr<apigate::StructuredLogger> logger_;
    std::shared_ptr<apigate::detail::BufferBudgetState> budget_{
        std::make_shared<apigate::detail::BufferBudgetState>(response_bytes)};
    std::shared_ptr<apigate::detail::ProxyAdmissionState> admission_{
        std::make_shared<apigate::detail::ProxyAdmissionState>(32)};
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<apigate::detail::SessionWriteProbe> probes_;
    std::vector<std::size_t> close_counts_;
    std::vector<std::shared_ptr<UpstreamPeer>> upstream_peers_;
    const std::string expected_body_{std::string{"write-body-marker"} +
                                     std::string(response_bytes - 17, 'w')};
    std::size_t upstream_writes_completed_{0};
};

TEST_F(SessionWriteRegressionTest, HoldsFullResponseBudgetUntilDownstreamWriteCompletes) {
    auto& holder = start_client(true);
    ASSERT_TRUE(run_until([&holder] { return holder.header_done; }));
    ASSERT_FALSE(holder.error);
    assert_blocked_write();
    assert_budget_rejection();
    EXPECT_TRUE(probes_.front().snapshot().pending);
    read_body(0);
    ASSERT_TRUE(run_until([&holder] { return holder.done; }));
    ASSERT_FALSE(holder.error);
    EXPECT_TRUE(holder.parser.get().body() == expected_body_);
    assert_full_recovery();
}

TEST_F(SessionWriteRegressionTest, ResetDuringPendingWriteReleasesBudgetExactlyOnce) {
    auto& holder = start_client(true);
    ASSERT_TRUE(run_until([&holder] { return holder.header_done; }));
    ASSERT_FALSE(holder.error);
    assert_blocked_write();
    assert_budget_rejection();
    holder.socket.set_option(asio::socket_base::linger(true, 0));
    close_socket(holder.socket);
    ASSERT_TRUE(run_until([this] { return close_counts_[0] == 1; }));
    probes_.front().stop();
    probes_.front().stop();
    assert_full_recovery();
    EXPECT_EQ(close_counts_[0], 1U);
}

TEST_F(SessionWriteRegressionTest, CancellationDuringPendingWriteDoesNotDoubleRelease) {
    auto& holder = start_client(true);
    ASSERT_TRUE(run_until([&holder] { return holder.header_done; }));
    assert_blocked_write();
    probes_.front().stop();
    probes_.front().stop();
    // Drop the test's extra shared ownership; Asio retains the session until its
    // cancelled write handler has run, matching the production lifetime.
    probes_.clear();
    assert_full_recovery();
    EXPECT_EQ(close_counts_[0], 1U);
}

}  // namespace
