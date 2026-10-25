#ifndef APIGATE_STREAM_RESPONSE_HPP_
#define APIGATE_STREAM_RESPONSE_HPP_

#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "apigate/http_handler.hpp"
#include "buffer_budget.hpp"

namespace apigate::detail
{
// One exchange owns one block. Async reads/writes retain it through cancellation.
struct StreamBlock
{
    std::vector<char> bytes;
    BufferBudgetPermit permit;
};
using StreamResume = std::function<void(const boost::system::error_code&)>;

// Serializer and message have a common lifetime, retained by every pending write.
struct StreamWriter
{
    explicit StreamWriter(HttpResponse header)
    {
        response.base() = std::move(header.base());
    }
    boost::beast::http::response<boost::beast::http::buffer_body> response;
    boost::beast::http::response_serializer<boost::beast::http::buffer_body> serializer{response};
};
}  // namespace apigate::detail
#endif  // APIGATE_STREAM_RESPONSE_HPP_
