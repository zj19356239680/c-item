#include "apigate/logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace apigate {
namespace {

[[nodiscard]] spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace:
            return spdlog::level::trace;
        case LogLevel::debug:
            return spdlog::level::debug;
        case LogLevel::info:
            return spdlog::level::info;
        case LogLevel::warn:
            return spdlog::level::warn;
        case LogLevel::error:
            return spdlog::level::err;
        case LogLevel::critical:
            return spdlog::level::critical;
    }
    return spdlog::level::info;
}

}  // namespace

StructuredLogger::StructuredLogger(const AppConfig& config)
    : service_name_(config.service_name),
      environment_(config.environment),
      logger_(std::make_shared<spdlog::logger>("apigate",
                                               std::make_shared<spdlog::sinks::stdout_sink_mt>())) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        R"({"timestamp":"%Y-%m-%dT%H:%M:%S.%eZ","level":"%l","payload":%v})",
        spdlog::pattern_time_type::utc);
    logger_->set_formatter(std::move(formatter));
    logger_->set_level(to_spdlog_level(config.log_level));
    logger_->flush_on(spdlog::level::warn);
}

nlohmann::json StructuredLogger::make_payload(std::string_view event,
                                              const nlohmann::json& fields) const {
    if (event.empty()) {
        throw std::invalid_argument("log event must not be empty");
    }
    if (!fields.is_null() && !fields.is_object()) {
        throw std::invalid_argument("log fields must be a JSON object");
    }

    nlohmann::json payload = fields.is_null() ? nlohmann::json::object() : fields;
    payload["service"] = service_name_;
    payload["environment"] = environment_;
    payload["event"] = event;
    return payload;
}

void StructuredLogger::info(std::string_view event, const nlohmann::json& fields) {
    logger_->info("{}", make_payload(event, fields).dump());
}

void StructuredLogger::warn(std::string_view event, const nlohmann::json& fields) {
    logger_->warn("{}", make_payload(event, fields).dump());
}

void StructuredLogger::error(std::string_view event, const nlohmann::json& fields) {
    logger_->error("{}", make_payload(event, fields).dump());
}

void StructuredLogger::critical(std::string_view event, const nlohmann::json& fields) {
    logger_->critical("{}", make_payload(event, fields).dump());
}

void StructuredLogger::write_configuration_valid(const nlohmann::json& fields) {
    auto result_logger = logger_->clone("apigate-configuration-check");
    result_logger->set_level(spdlog::level::info);
    result_logger->info("{}", make_payload("configuration_valid", fields).dump());
    result_logger->flush();
}

}  // namespace apigate
