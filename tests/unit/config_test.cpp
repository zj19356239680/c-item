#include "apigate/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <nlohmann/json.hpp>
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
        unsetenv("APIGATE_MAX_IN_FLIGHT_BYTES");
        unsetenv("APIGATE_SHUTDOWN_GRACE_MS");
        unsetenv("APIGATE_UPSTREAM_HOST");
        unsetenv("APIGATE_UPSTREAM_PORT");
        unsetenv("APIGATE_UPSTREAM_TIMEOUT_MS");
        unsetenv("APIGATE_ROUTES");
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
    EXPECT_EQ(config.max_in_flight_bytes, 67108864U);
    EXPECT_EQ(config.shutdown_grace_ms, 5000U);
    EXPECT_FALSE(config.upstream.has_value());
    EXPECT_TRUE(config.routes.empty());
}

TEST_F(ConfigTest, ParsesStaticRoutesWithDefaultsAndBoundaries) {
    const auto input = R"([{"prefix":"/","host":"localhost","port":1},
        {"prefix":"/users/Admin-._~9","host":"::1","port":65535,"timeout_ms":60000},
        {"prefix":"/orders","host":"127.0.0.1","port":8080,"timeout_ms":1}])";
    ASSERT_EQ(setenv("APIGATE_ROUTES", input, 1), 0);
    const auto config = apigate::load_config_from_environment();
    ASSERT_EQ(config.routes.size(), 3U);
    EXPECT_FALSE(config.upstream.has_value());
    EXPECT_EQ(config.routes[0].prefix, "/");
    EXPECT_FALSE(config.routes[0].strip_prefix);
    EXPECT_EQ(config.routes[0].upstream.timeout_ms, 3000U);
    EXPECT_EQ(config.routes[1].upstream.host, "::1");
    EXPECT_EQ(config.routes[1].upstream.port, 65535);
    EXPECT_EQ(config.routes[1].upstream.timeout_ms, 60000U);
    EXPECT_EQ(config.routes[2].upstream.timeout_ms, 1U);
}

TEST_F(ConfigTest, AcceptsOnlyBooleanStripPrefixAndRejectsDuplicates) {
    for (const bool strip : {false, true}) {
        const auto input =
            nlohmann::json::array(
                {{{"prefix", "/users"}, {"host", "h"}, {"port", 80}, {"strip_prefix", strip}}})
                .dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        const auto config = apigate::load_config_from_environment();
        ASSERT_EQ(config.routes.size(), 1U);
        EXPECT_EQ(config.routes.front().strip_prefix, strip);
    }
    for (const auto& value : nlohmann::json::array({0, 1, -1, 1.0, "true", "false", nullptr})) {
        const auto input =
            nlohmann::json::array(
                {{{"prefix", "/users"}, {"host", "h"}, {"port", 80}, {"strip_prefix", value}}})
                .dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
    ASSERT_EQ(
        setenv(
            "APIGATE_ROUTES",
            R"([{"prefix":"/users","host":"h","port":80,"strip_prefix":true,"strip_prefix":false}])",
            1),
        0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsRouteShapesFieldsDuplicatesAndTypes) {
    const char* invalid[] = {
        "",
        " ",
        "[",
        "[]",
        "{}",
        "null",
        "[true]",
        "[[]]",
        "[{}]",
        R"([{"prefix":"/a","host":"h"}])",
        R"([{"prefix":"/a","port":80}])",
        R"([{"host":"h","port":80}])",
        R"([{"prefix":"/a","host":"h","port":80,"extra":1}])",
        R"([{"prefix":"/a","prefix":"/b","host":"h","port":80}])",
        R"([{"prefix":"/a","host":"h","\u0068ost":"other","port":80}])",
        R"([{"prefix":"/a","host":"h","port":80,"port":81}])",
        R"([{"prefix":"/a","host":"h","port":80,"timeout_ms":1,"timeout_ms":2}])",
        R"([{"prefix":"/a","host":"h","port":80},{"prefix":"/a","host":"x","port":81}])",
        R"([{"prefix":false,"host":"h","port":80}])",
        R"([{"prefix":"/a","host":1,"port":80}])",
        R"([{"prefix":"/a","host":"","port":80}])",
        R"([{"prefix":"/a","host":"host/path","port":80}])",
        R"([{"prefix":"/a","host":"h","port":{"nested":1}}])",
        R"([{"prefix":"/a","host":"h","port":null}])",
    };
    for (const auto* input : invalid) {
        ASSERT_EQ(setenv("APIGATE_ROUTES", input, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
    for (const char* field : {"port", "timeout_ms"}) {
        for (const auto& invalid_number :
             nlohmann::json::array({0, -1, 65536, 1.0, 1.5, "80", true, nullptr})) {
            auto entry = nlohmann::json{{"prefix", "/a"}, {"host", "h"}, {"port", 80}};
            entry[field] = invalid_number;
            const auto input = nlohmann::json::array({entry}).dump();
            ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
            EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                         apigate::ConfigError);
        }
    }
}

TEST_F(ConfigTest, RejectsUnsafeRoutePrefixes) {
    const char* invalid[] = {"",        "users", "/a/",    "/a//b", "/a/./b",
                             "/a/../b", "/.",    "/..",    "/a?x",  "/a#x",
                             "/a b",    "/a\\b", "/a%20b", "/a\n",  "/caf\xc3\xa9"};
    for (const auto* prefix : invalid) {
        const auto input =
            nlohmann::json::array({{{"prefix", prefix}, {"host", "h"}, {"port", 80}}}).dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, EnforcesRouteCountAndRawByteBoundaries) {
    auto entries = nlohmann::json::array();
    for (int index = 0; index < 32; ++index) {
        entries.push_back({{"prefix", "/p" + std::to_string(index)}, {"host", "h"}, {"port", 80}});
    }
    auto input = entries.dump();
    ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().routes.size(), 32U);
    entries.push_back({{"prefix", "/last"}, {"host", "h"}, {"port", 80}});
    input = entries.dump();
    ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    input = R"([{"prefix":"/","host":"h","port":80}])";
    input.resize(std::size_t{64} * 1024, ' ');
    ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().routes.size(), 1U);
    input.push_back(' ');
    ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    ASSERT_EQ(
        setenv("APIGATE_ROUTES", R"([{"prefix":"/","host":"h","port":18446744073709551616}])", 1),
        0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsEveryLegacyConflictEvenEmptyValuesWithoutEchoingRoutes) {
    const std::string input =
        R"([{"prefix":"/private-prefix","host":"private-host.test","port":80}])";
    for (const char* variable :
         {"APIGATE_UPSTREAM_HOST", "APIGATE_UPSTREAM_PORT", "APIGATE_UPSTREAM_TIMEOUT_MS"}) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        ASSERT_EQ(setenv(variable, "", 1), 0);
        try {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "conflicting configuration was accepted";
        } catch (const apigate::ConfigError& error) {
            EXPECT_STREQ(error.what(), "APIGATE_ROUTES conflicts with legacy upstream variables");
        }
    }
    clear_environment();
    for (const auto& invalid :
         {input.substr(0, input.size() - 1),
          std::string{R"([{"prefix":"/private-prefix?x","host":"private-host.test","port":80}])"},
          std::string{
              R"([{"prefix":"/private-prefix","host":"private-host.test/secret","port":80}])"}}) {
        ASSERT_EQ(setenv("APIGATE_ROUTES", invalid.c_str(), 1), 0);
        try {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid private configuration was accepted";
        } catch (const apigate::ConfigError& error) {
            EXPECT_EQ(std::string{error.what()}.find("private-"), std::string::npos);
        }
    }
}

TEST_F(ConfigTest, AcceptsInFlightByteBudgetBoundariesAndIntermediateValue) {
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "1", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 1U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "123456789", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 123456789U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "1073741824", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 1073741824U);
}

TEST_F(ConfigTest, RejectsInvalidInFlightByteBudgetWithoutEchoingValues) {
    const char* invalid_values[] = {"",         "0",   "-1",  "1073741825",
                                    "12suffix", " 12", "12 ", "18446744073709551616"};
    for (const char* invalid_value : invalid_values) {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", invalid_value, 1), 0);
        try {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid in-flight byte budget was accepted";
        } catch (const apigate::ConfigError& error) {
            if (*invalid_value == '\0') {
                EXPECT_STREQ(error.what(), "APIGATE_MAX_IN_FLIGHT_BYTES must not be empty");
            } else {
                EXPECT_STREQ(error.what(),
                             "APIGATE_MAX_IN_FLIGHT_BYTES must be an integer from 1 to "
                             "1073741824");
            }
            if (std::string_view{invalid_value}.size() > 1) {
                EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
            }
        }
    }
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
