#ifndef APIGATE_API_KEY_AUTH_HPP_
#define APIGATE_API_KEY_AUTH_HPP_

#include <string_view>

#include "apigate/config.hpp"
#include "apigate/http_handler.hpp"

namespace apigate::detail
{
[[nodiscard]] bool valid_api_key_reference(std::string_view reference) noexcept;
[[nodiscard]] bool valid_api_key(std::string_view key) noexcept;
[[nodiscard]] ApiKeyAuthConfig hash_api_key(std::string_view key);
[[nodiscard]] bool authenticate_api_key(const ApiKeyAuthConfig& config, const HttpRequest& request);
}  // namespace apigate::detail
#endif  // APIGATE_API_KEY_AUTH_HPP_
