#include "request_id.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "http_proxy.hpp"

namespace
{
using apigate::detail::RequestIds;
bool fill(std::span<unsigned char> bytes)
{
    for (auto& byte : bytes)
    {
        byte = 0xab;
    }
    return true;
}
TEST(RequestIdTest, FixedFormatAndConsecutiveValuesAreDistinct)
{
    RequestIds ids{fill};
    const auto first = ids.next();
    EXPECT_EQ(first, "abababababababababababababababab-0000000000000001");
    EXPECT_EQ(first.size(), 49U);
    EXPECT_NE(first, ids.next());
}
TEST(RequestIdTest, RandomFailureAndSequenceExhaustionFailClosed)
{
    EXPECT_THROW(RequestIds(
                     [](std::span<unsigned char>)
                     {
                         return false;
                     }),
                 std::runtime_error);
    RequestIds ids{fill, std::numeric_limits<std::uint64_t>::max() - 1};
    EXPECT_EQ(ids.next().substr(33), "ffffffffffffffff");
    EXPECT_THROW(static_cast<void>(ids.next()), std::overflow_error);
    EXPECT_THROW(static_cast<void>(ids.next()), std::overflow_error);
}
TEST(RequestIdTest, ReplacesDuplicatesAndConnectionNominatedClientIds)
{
    apigate::HttpRequest request{boost::beast::http::verb::get, "/", 11};
    request.insert("X-Request-ID", "client-one");
    request.insert("x-request-id", "client-two");
    request.set(boost::beast::http::field::connection, "X-Request-ID");
    const apigate::UpstreamConfig backend{"localhost", 1};
    const auto upstream =
        apigate::detail::make_upstream_request({backend, {}}, request, false, "gateway");
    EXPECT_EQ(upstream.count("X-Request-ID"), 1U);
    EXPECT_EQ(upstream["X-Request-ID"], "gateway");
    apigate::detail::set_request_id(request.base(), "gateway");
    EXPECT_EQ(request.count("X-Request-ID"), 1U);
    EXPECT_EQ(request["X-Request-ID"], "gateway");
}
}  // namespace
