#include "upstream_pool.hpp"

#include <gtest/gtest.h>
#include <poll.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <memory>
#include <vector>

#include "http_proxy.hpp"

namespace
{
using namespace std::chrono_literals;
using apigate::detail::UpstreamConnection;
using apigate::detail::UpstreamPool;
using tcp = boost::asio::ip::tcp;

class UpstreamPoolTest : public testing::Test
{
   protected:
    boost::asio::io_context io_;
    tcp::acceptor acceptor_{io_, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    apigate::UpstreamConfig backend_{"127.0.0.1", acceptor_.local_endpoint().port()};
    std::vector<tcp::socket> peers_;
    std::shared_ptr<UpstreamPool> pool_{std::make_shared<UpstreamPool>(io_.get_executor())};

    static void expect_closed(tcp::socket& peer)
    {
        pollfd descriptor{peer.native_handle(), POLLIN, 0};
        ASSERT_EQ(::poll(&descriptor, 1, 1000), 1);
        char byte{};
        boost::system::error_code error;
        EXPECT_EQ(peer.read_some(boost::asio::buffer(&byte, 1), error), 0U);
        EXPECT_EQ(error, boost::asio::error::eof);
    }

    apigate::detail::UpstreamLease connect()
    {
        auto lease = std::make_unique<UpstreamConnection>(io_.get_executor(), backend_, nullptr);
        lease->transport().socket().connect(acceptor_.local_endpoint());
        peers_.push_back(acceptor_.accept());
        return lease;
    }
};

TEST_F(UpstreamPoolTest, LeasesAreExclusiveKeysAreExactAndStopIsIdempotent)
{
    auto lease = connect();
    auto* identity = lease.get();
    const auto now = UpstreamPool::Clock::now();
    pool_->put(std::move(lease), now);
    EXPECT_EQ(pool_->idle_count(), 1U);
    auto other = backend_;
    other.host = "localhost";
    EXPECT_FALSE(pool_->take(other, nullptr, now));
    other = backend_;
    ++other.port;
    EXPECT_FALSE(pool_->take(other, nullptr, now));
    other = backend_;
    other.scheme = apigate::UpstreamScheme::https;
    EXPECT_FALSE(pool_->take(other, nullptr, now));
    auto context = std::make_shared<apigate::UpstreamTlsContext>();
    EXPECT_FALSE(pool_->take(backend_, context, now));
    UpstreamConnection tls{io_.get_executor(), other, context};
    EXPECT_TRUE(tls.matches(other, context));
    EXPECT_FALSE(tls.matches(other, std::make_shared<apigate::UpstreamTlsContext>()));
    lease = pool_->take(backend_, nullptr, now);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease.get(), identity);
    EXPECT_FALSE(pool_->take(backend_, nullptr, now));
    EXPECT_EQ(pool_->idle_count(), 0U);
    pool_->stop();
    pool_->stop();
    EXPECT_TRUE(lease->transport().socket().is_open());  // stop doesn't cancel active work
    pool_->put(std::move(lease), now);
    EXPECT_EQ(pool_->idle_count(), 0U);
    EXPECT_FALSE(pool_->take(backend_, nullptr, now));
    expect_closed(peers_[0]);
}

TEST_F(UpstreamPoolTest, DiscardsDataArrivingWhileIdleAndFindsAnotherCleanCandidate)
{
    auto dirty = connect();
    const auto dirty_socket = dirty->transport().socket().native_handle();
    pool_->put(std::move(dirty));
    auto clean = connect();
    auto* clean_identity = clean.get();
    pool_->put(std::move(clean));
    ASSERT_EQ(pool_->idle_count(), 2U);
    const std::string extra = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\npoison";
    ASSERT_EQ(boost::asio::write(peers_[0], boost::asio::buffer(extra)), extra.size());
    pollfd descriptor{dirty_socket, POLLIN, 0};
    ASSERT_EQ(::poll(&descriptor, 1, 1000), 1);  // bytes ready, without consuming them
    auto lease = pool_->take(backend_, nullptr);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease.get(), clean_identity);
    EXPECT_EQ(pool_->idle_count(), 0U);
    lease.reset();
    for (auto& peer : peers_)
    {
        expect_closed(peer);
    }
}

TEST_F(UpstreamPoolTest, GlobalCapAndExplicitExpiryCloseIdleTransports)
{
    const auto now = UpstreamPool::Clock::now();
    for (std::size_t count = 0; count < UpstreamPool::max_idle + 1; ++count)
    {
        pool_->put(connect(), now);
    }
    EXPECT_EQ(pool_->idle_count(), 32U);
    pool_->expire(now + 30s - 1ns);
    EXPECT_EQ(pool_->idle_count(), 32U);
    pool_->expire(now + 30s);
    EXPECT_EQ(pool_->idle_count(), 0U);
    EXPECT_FALSE(pool_->take(backend_, nullptr, now + 30s));
    for (auto& peer : peers_)
    {
        expect_closed(peer);  // Includes the 33rd lease discarded at the global cap.
    }
    pool_->put(connect(), now);
    EXPECT_FALSE(pool_->take(backend_, nullptr, now + 30s));  // take also checks expiry
    EXPECT_EQ(pool_->idle_count(), 0U);
    pool_->stop();
    io_.run();  // cancelled cleanup task does not hold the process alive
}

TEST_F(UpstreamPoolTest, CleanupRunsWithoutBorrowingAndDestructorDiscardsLeases)
{
    // Use an explicit past insertion time so the real timer is already due; no 30-second sleep.
    pool_->put(connect(), UpstreamPool::Clock::now() - 31s);
    EXPECT_EQ(pool_->idle_count(), 1U);
    io_.run_for(100ms);
    EXPECT_EQ(pool_->idle_count(), 0U);
    auto lease = connect();
    lease->close();
    lease->close();
    pool_->put(std::move(lease));
    EXPECT_EQ(pool_->idle_count(), 0U);
    pool_->put(connect());
    EXPECT_EQ(pool_->idle_count(), 1U);
    pool_.reset();
    io_.restart();
    io_.run();
    for (auto& peer : peers_)
    {
        expect_closed(peer);
    }
}

TEST_F(UpstreamPoolTest, ProtocolOptInDoesNotForwardDownstreamConnectionTokens)
{
    apigate::HttpRequest request{boost::beast::http::verb::post, "/item?q=raw", 11};
    request.body() = "payload";
    request.set(boost::beast::http::field::connection, "close, X-Private-Hop");
    request.set("X-Private-Hop", "private");
    const auto upstream = apigate::detail::make_upstream_request({backend_, {}}, request, true);
    EXPECT_TRUE(upstream.keep_alive());
    EXPECT_EQ(upstream.count(boost::beast::http::field::connection), 0U);
    EXPECT_EQ(upstream.count("X-Private-Hop"), 0U);
    EXPECT_EQ(upstream.body(), "payload");
    EXPECT_EQ(upstream.target(), "/item?q=raw");
    EXPECT_FALSE(apigate::detail::make_upstream_request({backend_, {}}, request).keep_alive());
}
}  // namespace
