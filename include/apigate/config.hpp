#ifndef APIGATE_CONFIG_HPP_
#define APIGATE_CONFIG_HPP_

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace apigate {

enum class LogLevel : std::uint8_t {
    trace,
    debug,
    info,
    warn,
    error,
    critical,
};

struct AppConfig {
    std::string service_name{"api-gate"};
    std::string environment{"development"};
    LogLevel log_level{LogLevel::info};
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{8080};
};

class ConfigError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] AppConfig load_config_from_environment();
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

}  // namespace apigate

#endif  // APIGATE_CONFIG_HPP_
