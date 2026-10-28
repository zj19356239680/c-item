#ifndef APIGATE_REQUEST_ID_HPP_
#define APIGATE_REQUEST_ID_HPP_
#include <boost/beast/http/fields.hpp>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace apigate::detail
{
class RequestIds
{
   public:
    using RandomFill = std::function<bool(std::span<unsigned char>)>;
    explicit RequestIds(const RandomFill& random = {}, std::uint64_t initial_sequence = 0);
    [[nodiscard]] std::string next();

   private:
    std::string prefix_;
    std::uint64_t sequence_;
};
void set_request_id(boost::beast::http::fields& fields, std::string_view id);
}  // namespace apigate::detail
#endif
