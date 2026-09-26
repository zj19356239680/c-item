#include "apigate/logging.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

TEST(StructuredLoggerTest, HonorsConfiguredThreshold) {
    apigate::AppConfig config;
    config.log_level = apigate::LogLevel::warn;

    testing::internal::CaptureStdout();
    {
        apigate::StructuredLogger logger{config};
        logger.info("filtered_info");
        logger.warn("visible_warning");
    }
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(output.find("filtered_info"), std::string::npos);
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(output.find('\n'), output.size() - 1);
    const auto record = nlohmann::json::parse(output);
    EXPECT_EQ(record.at("level"), "warning");
    EXPECT_EQ(record.at("payload").at("event"), "visible_warning");
}

}  // namespace
