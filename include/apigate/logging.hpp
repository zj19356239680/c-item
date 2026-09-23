#ifndef APIGATE_LOGGING_HPP_
#define APIGATE_LOGGING_HPP_

#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>

#include "apigate/config.hpp"

namespace spdlog {
class logger;  // NOLINT(readability-identifier-naming): name is fixed by spdlog's API.
}

namespace apigate {

class StructuredLogger {
   public:
    explicit StructuredLogger(const AppConfig& config);

    void info(std::string_view event, const nlohmann::json& fields = {});
    void warn(std::string_view event, const nlohmann::json& fields = {});
    void error(std::string_view event, const nlohmann::json& fields = {});
    void critical(std::string_view event, const nlohmann::json& fields = {});

   private:
    [[nodiscard]] nlohmann::json make_payload(std::string_view event,
                                              const nlohmann::json& fields) const;

    std::string service_name_;
    std::string environment_;
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace apigate

#endif  // APIGATE_LOGGING_HPP_
