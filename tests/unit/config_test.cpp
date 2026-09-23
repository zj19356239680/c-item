#include "apigate/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

// These tests run serially in one process and restore the environment after each case.
// NOLINTBEGIN(concurrency-mt-unsafe)
class ConfigTest : public testing::Test {
   protected:
    void SetUp() override { clear_environment(); }

    void TearDown() override { clear_environment(); }

    static void clear_environment() {
        unsetenv("APIGATE_SERVICE_NAME");
        unsetenv("APIGATE_ENVIRONMENT");
        unsetenv("APIGATE_LOG_LEVEL");
        unsetenv("APIGATE_LISTEN_ADDRESS");
        unsetenv("APIGATE_LISTEN_PORT");
    }
};

TEST_F(ConfigTest, UsesSafeDefaults) {
    const auto config = apigate::load_config_from_environment();

    EXPECT_EQ(config.service_name, "api-gate");
    EXPECT_EQ(config.environment, "development");
    EXPECT_EQ(config.log_level, apigate::LogLevel::info);
    EXPECT_EQ(config.listen_address, "127.0.0.1");
    EXPECT_EQ(config.listen_port, 8080);
}

TEST_F(ConfigTest, ReadsValidOverrides) {
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", "edge-gateway", 1), 0);
    ASSERT_EQ(setenv("APIGATE_ENVIRONMENT", "staging.cn", 1), 0);
    ASSERT_EQ(setenv("APIGATE_LOG_LEVEL", "debug", 1), 0);
    ASSERT_EQ(setenv("APIGATE_LISTEN_ADDRESS", "::1", 1), 0);
    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "0", 1), 0);

    const auto config = apigate::load_config_from_environment();

    EXPECT_EQ(config.service_name, "edge-gateway");
    EXPECT_EQ(config.environment, "staging.cn");
    EXPECT_EQ(config.log_level, apigate::LogLevel::debug);
    EXPECT_EQ(config.listen_address, "::1");
    EXPECT_EQ(config.listen_port, 0);
}

TEST_F(ConfigTest, AcceptsMaximumLengthLabel) {
    const std::string service_name(64, 'a');
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", service_name.c_str(), 1), 0);

    const auto config = apigate::load_config_from_environment();

    EXPECT_EQ(config.service_name, service_name);
}

TEST_F(ConfigTest, RejectsLabelLongerThanMaximum) {
    const std::string service_name(65, 'a');
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", service_name.c_str(), 1), 0);

    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsUnknownLogLevel) {
    ASSERT_EQ(setenv("APIGATE_LOG_LEVEL", "verbose", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsUnsafeServiceName) {
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", "gateway with spaces", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsEmptyValue) {
    ASSERT_EQ(setenv("APIGATE_ENVIRONMENT", "", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsInvalidListenAddress) {
    ASSERT_EQ(setenv("APIGATE_LISTEN_ADDRESS", "localhost", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsInvalidListenPort) {
    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "65536", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);

    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "80suffix", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);

    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "-1", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}
// NOLINTEND(concurrency-mt-unsafe)

}  // namespace
