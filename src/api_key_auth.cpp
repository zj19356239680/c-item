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
ApiKeyDigest hash_api_key(std::string_view key)
{
    ApiKeyDigest result{};
    unsigned int length = 0;
    if (EVP_Digest(key.data(), key.size(), result.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != result.size())
    {
        throw std::runtime_error("API key digest calculation failed");
    }
    return result;
}
bool matches_api_key_digest(const ApiKeyAuthConfig& config, const ApiKeyDigest& candidate) noexcept
{
    if (config.key_count == 0 || config.key_count > config.digests.size())
    {
        return false;
    }
    unsigned int matched = 0;
    for (std::size_t index = 0; index < config.key_count; ++index)
    {
        // Always compare the whole configured set, independent of the matching position.
        matched |= static_cast<unsigned int>(
            CRYPTO_memcmp(config.digests[index].data(), candidate.data(), candidate.size()) == 0);
    }
    return matched != 0;
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
    return matches_api_key_digest(config, candidate);
}
}  // namespace apigate::detail
