#include "apigate/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <string_view>

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
        unsetenv("APIGATE_MAX_CONNECTIONS");
        unsetenv("APIGATE_MAX_CONCURRENT_PROXIES");
        unsetenv("APIGATE_SHUTDOWN_GRACE_MS");
        unsetenv("APIGATE_UPSTREAM_HOST");
        unsetenv("APIGATE_UPSTREAM_PORT");
        unsetenv("APIGATE_UPSTREAM_TIMEOUT_MS");
    }
};

TEST_F(ConfigTest, UsesSafeDefaults) {
    const auto config = apigate::load_config_from_environment();

    EXPECT_EQ(config.service_name, "api-gate");
    EXPECT_EQ(config.environment, "development");
    EXPECT_EQ(config.log_level, apigate::LogLevel::info);
    EXPECT_EQ(config.listen_address, "127.0.0.1");
    EXPECT_EQ(config.listen_port, 8080);
    EXPECT_EQ(config.max_connections, 256);
    EXPECT_EQ(config.max_concurrent_proxies, 32);
    EXPECT_EQ(config.shutdown_grace_ms, 5000U);
    EXPECT_FALSE(config.upstream.has_value());
}

TEST_F(ConfigTest, AcceptsShutdownGraceBoundaries) {
    ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", "1", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().shutdown_grace_ms, 1U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", "60000", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().shutdown_grace_ms, 60000U);
}

TEST_F(ConfigTest, RejectsInvalidShutdownGraceWithoutEchoingValues) {
    const char* invalid_values[] = {"0", "-1", "60001", "12suffix", " 12", "12 ", ""};
    for (const char* invalid_value : invalid_values) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", invalid_value, 1), 0);
        try {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid shutdown grace was accepted";
        } catch (const apigate::ConfigError& error) {
            if (*invalid_value == '\0') {
                EXPECT_STREQ(error.what(), "APIGATE_SHUTDOWN_GRACE_MS must not be empty");
            } else {
                EXPECT_STREQ(error.what(),
                             "APIGATE_SHUTDOWN_GRACE_MS must be an integer from 1 to 60000");
            }
            if (std::string_view{invalid_value}.size() > 1) {
                EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
            }
        }
    }
}

TEST_F(ConfigTest, AcceptsCapacityLimitBoundaries) {
    ASSERT_EQ(setenv("APIGATE_MAX_CONNECTIONS", "1", 1), 0);
    ASSERT_EQ(setenv("APIGATE_MAX_CONCURRENT_PROXIES", "65535", 1), 0);

    const auto first_config = apigate::load_config_from_environment();

    EXPECT_EQ(first_config.max_connections, 1);
    EXPECT_EQ(first_config.max_concurrent_proxies, 65535);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_MAX_CONNECTIONS", "65535", 1), 0);
    ASSERT_EQ(setenv("APIGATE_MAX_CONCURRENT_PROXIES", "1", 1), 0);
    const auto second_config = apigate::load_config_from_environment();

    EXPECT_EQ(second_config.max_connections, 65535);
    EXPECT_EQ(second_config.max_concurrent_proxies, 1);
}

TEST_F(ConfigTest, RejectsInvalidCapacityLimitsWithoutEchoingValues) {
    const char* variable_names[] = {
        "APIGATE_MAX_CONNECTIONS",
        "APIGATE_MAX_CONCURRENT_PROXIES",
    };
    const char* invalid_values[] = {"0", "-1", "65536", "12suffix", ""};

    for (const char* variable_name : variable_names) {
        for (const char* invalid_value : invalid_values) {
            clear_environment();
            ASSERT_EQ(setenv(variable_name, invalid_value, 1), 0);
            try {
                static_cast<void>(apigate::load_config_from_environment());
                FAIL() << "invalid capacity limit was accepted";
            } catch (const apigate::ConfigError& error) {
                if (*invalid_value != '\0') {
                    EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
                }
            }
        }
    }
}

TEST_F(ConfigTest, EnablesStaticUpstreamWithDefaultTimeout) {
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8081", 1), 0);

    const auto config = apigate::load_config_from_environment();

    ASSERT_TRUE(config.upstream.has_value());
    const auto upstream = config.upstream.value_or(apigate::UpstreamConfig{});
    EXPECT_EQ(upstream.host, "upstream.internal");
    EXPECT_EQ(upstream.port, 8081);
    EXPECT_EQ(upstream.timeout_ms, 3000U);
}

TEST_F(ConfigTest, AcceptsIpLiteralsAndCustomTimeout) {
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "::1", 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "65535", 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_TIMEOUT_MS", "60000", 1), 0);

    const auto config = apigate::load_config_from_environment();

    ASSERT_TRUE(config.upstream.has_value());
    const auto upstream = config.upstream.value_or(apigate::UpstreamConfig{});
    EXPECT_EQ(upstream.host, "::1");
    EXPECT_EQ(upstream.port, 65535);
    EXPECT_EQ(upstream.timeout_ms, 60000U);
}

TEST_F(ConfigTest, RejectsIncompleteUpstreamConfiguration) {
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    clear_environment();

    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    clear_environment();

    ASSERT_EQ(setenv("APIGATE_UPSTREAM_TIMEOUT_MS", "100", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsEmptyOrUnsafeUpstreamHost) {
    const char* invalid_hosts[] = {"",          "host name",  "http://upstream",
                                   "host/path", "host?query", "host#fragment",
                                   "user@host", "-bad.host",  "bad-.host"};
    for (const char* host : invalid_hosts) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", host, 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, RejectsInvalidUpstreamPort) {
    const char* invalid_ports[] = {"0", "65536", "-1", "80suffix"};
    for (const char* port : invalid_ports) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", port, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, RejectsInvalidUpstreamTimeout) {
    const char* invalid_timeouts[] = {"0", "60001", "-1", "100suffix"};
    for (const char* timeout : invalid_timeouts) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_TIMEOUT_MS", timeout, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, DoesNotEchoInvalidUpstreamValue) {
    const std::string private_marker = "private-upstream-marker";
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", (private_marker + "/path").c_str(), 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);

    try {
        static_cast<void>(apigate::load_config_from_environment());
        FAIL() << "invalid upstream host was accepted";
    } catch (const apigate::ConfigError& error) {
        EXPECT_EQ(std::string{error.what()}.find(private_marker), std::string::npos);
    }
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
