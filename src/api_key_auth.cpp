#include "api_key_auth.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <boost/beast/core/string.hpp>
#include <iterator>
#include <stdexcept>

namespace apigate::detail
{
namespace
{
bool upper_digit_or_underscore(unsigned char character) noexcept
{
    return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
           character == '_';
}
}  // namespace

bool valid_api_key_reference(std::string_view reference) noexcept
{
    constexpr std::string_view prefix = "APIGATE_AUTH_KEY_";
    if (!reference.starts_with(prefix))
    {
        return false;
    }
    reference.remove_prefix(prefix.size());
    return !reference.empty() && reference.size() <= 64 &&
           std::ranges::all_of(reference, upper_digit_or_underscore);
}
bool valid_api_key(std::string_view key) noexcept
{
    return key.size() >= 32 && key.size() <= 256 &&
           std::ranges::all_of(key,
                               [](unsigned char character)
                               {
                                   return upper_digit_or_underscore(character) ||
                                          (character >= 'a' && character <= 'z') ||
                                          character == '-';
                               });
}
ApiKeyAuthConfig hash_api_key(std::string_view key)
{
    ApiKeyAuthConfig result;
    unsigned int length = 0;
    if (EVP_Digest(key.data(), key.size(), result.digest.data(), &length, EVP_sha256(), nullptr) !=
            1 ||
        length != result.digest.size())
    {
        throw std::runtime_error("API key digest calculation failed");
    }
    return result;
}
bool authenticate_api_key(const ApiKeyAuthConfig& config, const HttpRequest& request)
{
    namespace http = boost::beast::http;
    const auto [begin, end] = request.equal_range(http::field::authorization);
    if (begin == end || std::next(begin) != end)
    {
        return false;
    }
    std::string_view value{begin->value().data(), begin->value().size()};
    const auto separator = value.find(' ');
    if (separator == std::string_view::npos ||
        !boost::beast::iequals(value.substr(0, separator), "Bearer"))
    {
        return false;
    }
    value.remove_prefix(separator);
    const auto credential = value.find_first_not_of(' ');  // HTTP auth-scheme uses 1*SP.
    if (credential == std::string_view::npos)
    {
        return false;
    }
    value.remove_prefix(credential);
    if (!valid_api_key(value))
    {
        return false;
    }
    const auto candidate = hash_api_key(value);
    return CRYPTO_memcmp(config.digest.data(), candidate.digest.data(), config.digest.size()) == 0;
}
}  // namespace apigate::detail
