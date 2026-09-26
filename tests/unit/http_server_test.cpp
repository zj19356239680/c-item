#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include "http_server_internal.hpp"

namespace {

namespace asio = boost::asio;
using apigate::detail::classify_http_write_result;
using apigate::detail::HttpWriteDisposition;

TEST(HttpWriteDispositionTest, ClassifiesSuccessfulCompletion) {
    EXPECT_EQ(classify_http_write_result({}, false), HttpWriteDisposition::success);
}

TEST(HttpWriteDispositionTest, TreatsStoppingAsExpectedCancellation) {
    const boost::system::error_code unexpected_error = asio::error::network_down;

    EXPECT_EQ(classify_http_write_result(unexpected_error, true),
              HttpWriteDisposition::expected_cancellation);
}

TEST(HttpWriteDispositionTest, ClassifiesOperationAbortedAsExpectedCancellation) {
    const boost::system::error_code operation_aborted = asio::error::operation_aborted;

    EXPECT_EQ(classify_http_write_result(operation_aborted, false),
              HttpWriteDisposition::expected_cancellation);
}

TEST(HttpWriteDispositionTest, ClassifiesClientDisconnects) {
    const boost::system::error_code client_disconnects[] = {
        asio::error::broken_pipe,
        asio::error::connection_aborted,
        asio::error::connection_reset,
        asio::error::eof,
    };

    for (const auto& error : client_disconnects) {
        EXPECT_EQ(classify_http_write_result(error, false),
                  HttpWriteDisposition::client_disconnected);
    }
}

TEST(HttpWriteDispositionTest, ClassifiesUnexpectedErrors) {
    const boost::system::error_code unexpected_error = asio::error::network_down;

    EXPECT_EQ(classify_http_write_result(unexpected_error, false),
              HttpWriteDisposition::unexpected_error);
}

}  // namespace
