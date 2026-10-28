#include "request_id.hpp"

#include <openssl/rand.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace apigate::detail
{
namespace
{
constexpr std::string_view hex = "0123456789abcdef";
}
RequestIds::RequestIds(const RandomFill& random, std::uint64_t initial_sequence)
    : sequence_(initial_sequence)
{
    std::array<unsigned char, 16> bytes{};
    const bool ready =
        random ? random(bytes) : RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1;
    if (!ready)
    {
        throw std::runtime_error("request ID initialization failed");
    }
    for (const auto byte : bytes)
    {
        prefix_ += hex[byte >> 4U];
        prefix_ += hex[byte & 15U];
    }
}
std::string RequestIds::next()
{
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error("request ID sequence exhausted");
    }
    auto value = ++sequence_;
    std::string suffix(16, '0');
    for (auto position = suffix.rbegin(); position != suffix.rend(); ++position)
    {
        *position = hex[value & 15U];
        value >>= 4U;
    }
    return prefix_ + "-" + suffix;
}
void set_request_id(boost::beast::http::fields& fields, std::string_view id)
{
    fields.erase("X-Request-ID");
    if (!id.empty())
    {
        fields.set("X-Request-ID", id);
    }
}
}  // namespace apigate::detail
