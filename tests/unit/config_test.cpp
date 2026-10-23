#include "apigate/config.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>

#include "upstream_tls.hpp"

namespace
{

class TemporaryCaDirectory
{
   public:
    TemporaryCaDirectory()
    {
        auto pattern =
            (std::filesystem::temp_directory_path() / "apigate-config-ca-XXXXXX").string();
        const auto* directory = mkdtemp(pattern.data());
        if (directory == nullptr)
        {
            throw std::system_error(errno, std::generic_category(), "create test CA directory");
        }
        path_ = directory;
    }

    TemporaryCaDirectory(const TemporaryCaDirectory&) = delete;
    TemporaryCaDirectory& operator=(const TemporaryCaDirectory&) = delete;

    ~TemporaryCaDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error)
        {
            ADD_FAILURE() << "test CA directory cleanup failed: " << error.message();
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

   private:
    std::filesystem::path path_;
};

// These tests run serially in one process and restore the environment after each case.
// NOLINTBEGIN(concurrency-mt-unsafe)
class ConfigTest : public testing::Test
{
   protected:
    void SetUp() override
    {
        clear_environment();
    }

    void TearDown() override
    {
        clear_environment();
    }

    static void clear_environment()
    {
        unsetenv("APIGATE_SERVICE_NAME");
        unsetenv("APIGATE_ENVIRONMENT");
        unsetenv("APIGATE_LOG_LEVEL");
        unsetenv("APIGATE_LISTEN_ADDRESS");
        unsetenv("APIGATE_LISTEN_PORT");
        unsetenv("APIGATE_MAX_CONNECTIONS");
        unsetenv("APIGATE_MAX_CONCURRENT_PROXIES");
        unsetenv("APIGATE_MAX_IN_FLIGHT_BYTES");
        unsetenv("APIGATE_SHUTDOWN_GRACE_MS");
        unsetenv("APIGATE_UPSTREAM_KEEP_ALIVE");
        unsetenv("APIGATE_UPSTREAM_HOST");
        unsetenv("APIGATE_UPSTREAM_PORT");
        unsetenv("APIGATE_UPSTREAM_TIMEOUT_MS");
        unsetenv("APIGATE_UPSTREAM_CA_FILE");
        unsetenv("APIGATE_ROUTES");
        unsetenv("APIGATE_METRICS_ENABLED");
    }
};

TEST_F(ConfigTest, UsesSafeDefaults)
{
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
    EXPECT_FALSE(config.metrics_enabled);
    EXPECT_FALSE(config.upstream_keep_alive);
    EXPECT_FALSE(config.upstream.has_value());
    EXPECT_TRUE(config.routes.empty());
    EXPECT_FALSE(config.upstream_tls_context);
}

TEST_F(ConfigTest, UpstreamKeepAliveIsStrictAndDoesNotEchoInput)
{
    for (const auto* value : {"true", "false"})
    {
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_KEEP_ALIVE", value, 1), 0);
        EXPECT_EQ(apigate::load_config_from_environment().upstream_keep_alive,
                  std::string_view{value} == "true");
    }
    for (const auto* value : {"", "TRUE", "False", "0", " true", "false ", "private-marker"})
    {
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_KEEP_ALIVE", value, 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "expected invalid boolean rejection";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string_view{error.what()}.find("private-marker"),
                      std::string_view::npos);
        }
    }
}

TEST_F(ConfigTest, MetricsBooleanIsStrictAndDoesNotEchoInput)
{
    for (const auto* value : {"true", "false"})
    {
        ASSERT_EQ(setenv("APIGATE_METRICS_ENABLED", value, 1), 0);
        EXPECT_EQ(apigate::load_config_from_environment().metrics_enabled,
                  std::string_view{value} == "true");
    }
    for (const auto* value : {"", "TRUE", "False", "1", " true", "false ", "private-marker"})
    {
        ASSERT_EQ(setenv("APIGATE_METRICS_ENABLED", value, 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "expected invalid boolean rejection";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string_view{error.what()}.find("private-marker"),
                      std::string_view::npos);
        }
    }
}

TEST_F(ConfigTest, ParsesStaticRoutesWithDefaultsAndBoundaries)
{
    const auto input = R"([{"prefix":"/","host":"localhost","port":1},
        {"prefix":"/users/Admin-._~9","host":"::1","port":65535,"timeout_ms":60000},
        {"prefix":"/orders","host":"127.0.0.1","port":8080,"timeout_ms":1}])";
    ASSERT_EQ(setenv("APIGATE_ROUTES", input, 1), 0);
    const auto config = apigate::load_config_from_environment();
    ASSERT_EQ(config.routes.size(), 3U);
    EXPECT_FALSE(config.upstream.has_value());
    EXPECT_EQ(config.routes[0].prefix, "/");
    EXPECT_FALSE(config.routes[0].strip_prefix);
    EXPECT_FALSE(config.routes[0].rate_limit.has_value());
    EXPECT_FALSE(config.routes[0].health_check.has_value());
    ASSERT_EQ(config.routes[0].upstreams.size(), 1U);
    ASSERT_EQ(config.routes[1].upstreams.size(), 1U);
    ASSERT_EQ(config.routes[2].upstreams.size(), 1U);
    EXPECT_EQ(config.routes[0].upstreams.front().timeout_ms, 3000U);
    EXPECT_EQ(config.routes[1].upstreams.front().host, "::1");
    EXPECT_EQ(config.routes[1].upstreams.front().port, 65535);
    EXPECT_EQ(config.routes[1].upstreams.front().timeout_ms, 60000U);
    EXPECT_EQ(config.routes[2].upstreams.front().timeout_ms, 1U);
}

TEST_F(ConfigTest, ParsesHealthChecksDefaultsAndBoundariesForBothBackendForms)
{
    const auto raw =
        R"([{"prefix":"/a","host":"localhost","port":1,"health_check":{"path":"/live"}},
        {"prefix":"/b","upstreams":[{"host":"localhost","port":1}],"health_check":{
        "path":"/health/deep","interval_ms":100,"timeout_ms":50,"unhealthy_threshold":1,"healthy_threshold":10}},
        {"prefix":"/c","host":"localhost","port":1,"health_check":{
        "path":"/","interval_ms":60000,"timeout_ms":60000,"unhealthy_threshold":10,"healthy_threshold":1}}])";
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw, 1), 0);
    const auto config = apigate::load_config_from_environment();
    const auto& first = config.routes[0].health_check;
    const auto& second = config.routes[1].health_check;
    const auto& third = config.routes[2].health_check;
    if (!first || !second || !third)
    {
        FAIL() << "expected all configured health checks";
        return;
    }
    const auto& settings = *first;
    EXPECT_EQ(settings.path, "/live");
    EXPECT_EQ(settings.interval_ms, 5000U);
    EXPECT_EQ(settings.timeout_ms, 1000U);
    EXPECT_EQ(settings.unhealthy_threshold, 3U);
    EXPECT_EQ(settings.healthy_threshold, 2U);
    EXPECT_EQ(second->timeout_ms, 50U);
    EXPECT_EQ(third->timeout_ms, 60000U);
}

TEST_F(ConfigTest, RejectsInvalidHealthObjectsFieldsPathsNumbersAndDuplicatesSafely)
{
    using Json = nlohmann::json;
    const Json base{{"prefix", "/"}, {"host", "private-host.test"}, {"port", 1}};
    const auto reject = [&base](const Json& health)
    {
        auto route = base;
        route["health_check"] = health;
        const auto raw = Json::array({route}).dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid health configuration accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string_view{error.what()}.find("private-"), std::string_view::npos);
        }
    };
    for (const auto& invalid :
         {Json{}, Json(true), Json(1), Json("private-path"), Json::array(), Json::object()})
    {
        reject(invalid);
    }
    for (const auto* path : {"", "private-path", "https://private-host.test/", "/private-path?q",
                             "/private-path#f", "/private-path space", "/private-path\r",
                             "/private-path\n", "/private-path\t", "/private-path\x7f"})
    {
        reject(Json{{"path", path}});
    }
    for (const auto& path : {Json{}, Json(true), Json(1), Json::array(), Json::object()})
    {
        reject(Json{{"path", path}});
    }
    for (const auto* field :
         {"interval_ms", "timeout_ms", "unhealthy_threshold", "healthy_threshold"})
    {
        for (const auto& value : {Json{}, Json(true), Json(1.5), Json("1000"), Json(-1), Json(0),
                                  Json(1000000), Json::parse("18446744073709551616")})
        {
            reject(Json{{"path", "/private-health"}, {field, value}});
        }
    }
    reject(Json{{"path", "/private-health"}, {"interval_ms", 99}});
    reject(Json{{"path", "/private-health"}, {"timeout_ms", 49}});
    reject(Json{{"path", "/private-health"}, {"interval_ms", 100}, {"timeout_ms", 101}});
    reject(Json{{"path", "/private-health"}, {"unknown", 1}});
    for (const auto* fields :
         {R"("path":"/private-health","path":"/private-health")",
          R"("path":"/private-health","interval_ms":5000,"interval_ms":5000)",
          R"("path":"/private-health","timeout_ms":1000,"timeout_ms":1000)",
          R"("path":"/private-health","healthy_threshold":2,"healthy_threshold":2)",
          R"("path":"/private-health","unhealthy_threshold":3,"unhealthy_threshold":3)"})
    {
        const auto raw =
            std::string{R"([{"prefix":"/","host":"localhost","port":1,"health_check":{)"} + fields +
            "}}]";
        ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, ParsesSchemesWithoutPortInferenceAndEnforcesTlsPolicy)
{
    const auto raw = R"([{"prefix":"/a","host":"localhost","port":9001,"scheme":"https"},
        {"prefix":"/b","upstreams":[{"host":"localhost","port":443},
        {"host":"localhost","port":80,"scheme":"https"},
        {"host":"localhost","port":443,"scheme":"http"}]}])";
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw, 1), 0);
    const auto config = apigate::load_config_from_environment();
    EXPECT_EQ(config.routes[0].upstreams[0].scheme, apigate::UpstreamScheme::https);
    EXPECT_EQ(config.routes[1].upstreams[0].scheme, apigate::UpstreamScheme::http);
    EXPECT_EQ(config.routes[1].upstreams[1].scheme, apigate::UpstreamScheme::https);
    EXPECT_EQ(config.routes[1].upstreams[2].scheme, apigate::UpstreamScheme::http);
    ASSERT_TRUE(config.upstream_tls_context);
    auto* context = config.upstream_tls_context->context.native_handle();
    EXPECT_EQ(SSL_CTX_get_min_proto_version(context), TLS1_2_VERSION);
    EXPECT_EQ(SSL_CTX_get_verify_mode(context), SSL_VERIFY_PEER);
    EXPECT_EQ(SSL_CTX_get_session_cache_mode(context), SSL_SESS_CACHE_OFF);
}

TEST_F(ConfigTest, RejectsInvalidSchemesDuplicatesAndPoolConflicts)
{
    for (const auto* value : {"null", "true", "1", "1.0", "\"\"", "\"HTTPS\"", "\"ftp\""})
    {
        const std::string backend =
            std::string{R"({"host":"localhost","port":443,"scheme":)"} + value + "}";
        for (const auto& raw : {"[{\"prefix\":\"/\"," + backend.substr(1) + "]",
                                "[{\"prefix\":\"/\",\"upstreams\":[" + backend + "]}]"})
        {
            ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
            EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                         apigate::ConfigError);
        }
    }
    for (
        const auto* raw :
        {R"([{"prefix":"/","host":"localhost","port":443,"scheme":"http","scheme":"https"}])",
         R"([{"prefix":"/","upstreams":[{"host":"localhost","port":443,"scheme":"http","scheme":"https"}]}])",
         R"([{"prefix":"/","scheme":null,"upstreams":[{"host":"localhost","port":443}]}])",
         R"([{"prefix":"/","host":"localhost","scheme":"https"}])"})
    {
        ASSERT_EQ(setenv("APIGATE_ROUTES", raw, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, ExplicitCaMustLoadWithoutHttpsAndNeverEchoesPaths)
{
    const TemporaryCaDirectory directory;
    const auto empty_ca = directory.path() / "empty.pem";
    {
        std::ofstream output{empty_ca};
        ASSERT_TRUE(output.is_open());
    }
    for (const auto& path : {std::string{}, empty_ca.string(), directory.path().string(),
                             (directory.path() / "missing.pem").string()})
    {
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_CA_FILE", path.c_str(), 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid trust source was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            if (!path.empty())
            {
                EXPECT_EQ(std::string_view{error.what()}.find(path), std::string_view::npos);
            }
        }
    }
}

TEST_F(ConfigTest, ParsesUpstreamPoolsWithDefaultsAndPreservesDuplicateAddressSlots)
{
    const auto raw =
        R"([{"prefix":"/a","strip_prefix":true,"rate_limit":{"requests_per_second":10,"burst":20},
        "upstreams":[{"host":"::1","port":1},{"host":"::1","port":1,"timeout_ms":60000},
        {"host":"127.0.0.1","port":65535,"timeout_ms":1}]}])";
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw, 1), 0);
    const auto config = apigate::load_config_from_environment();
    ASSERT_EQ(config.routes.size(), 1U);
    const auto& route = config.routes.front();
    ASSERT_EQ(route.upstreams.size(), 3U);
    EXPECT_TRUE(route.strip_prefix);
    EXPECT_TRUE(route.rate_limit.has_value());
    EXPECT_EQ(route.upstreams[0].host, "::1");
    EXPECT_EQ(route.upstreams[0].port, 1);
    EXPECT_EQ(route.upstreams[0].timeout_ms, 3000U);
    EXPECT_EQ(route.upstreams[1].timeout_ms, 60000U);
    EXPECT_EQ(route.upstreams[2].port, 65535);
    EXPECT_EQ(route.upstreams[2].timeout_ms, 1U);
}

TEST_F(ConfigTest, EnforcesPoolRouteAndRawSizeLimitsTogether)
{
    auto pool = nlohmann::json::array();
    for (int slot = 0; slot < 16; ++slot)
    {
        pool.push_back({{"host", "h"}, {"port", 1}});
    }
    auto entries = nlohmann::json::array();
    for (int route = 0; route < 32; ++route)
    {
        entries.push_back({{"prefix", "/p" + std::to_string(route)},
                           {"upstreams", pool},
                           {"rate_limit", {{"requests_per_second", 1}, {"burst", 1}}}});
    }
    auto raw = entries.dump();
    raw.resize(std::size_t{64} * 1024, ' ');
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
    const auto config = apigate::load_config_from_environment();
    ASSERT_EQ(config.routes.size(), 32U);
    EXPECT_EQ(config.routes.back().upstreams.size(), 16U);
    raw.push_back(' ');
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    entries.push_back({{"prefix", "/extra"}, {"upstreams", pool}});
    raw = entries.dump();
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    pool.push_back({{"host", "h"}, {"port", 1}});
    raw = nlohmann::json::array({{{"prefix", "/a"}, {"upstreams", pool}}}).dump();
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsConflictingAndMalformedUpstreamPoolsWithoutEchoingValues)
{
    const auto base =
        nlohmann::json{{"prefix", "/private-prefix"},
                       {"upstreams", {{{"host", "private-host.test"}, {"port", 80}}}}};
    const auto reject = [](const std::string& raw)
    {
        ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid pool accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string{error.what()}.find("private-"), std::string::npos);
        }
    };
    for (const char* field : {"host", "port", "timeout_ms"})
    {
        auto entry = base;
        entry[field] = nullptr;
        reject(nlohmann::json::array({entry}).dump());
    }
    for (const auto& pool : nlohmann::json::array(
             {nullptr, true, 1, "private-value", nlohmann::json::object(), nlohmann::json::array(),
              nlohmann::json::array({nullptr}), nlohmann::json::array({nlohmann::json::array()}),
              nlohmann::json::array({{{"host", "h"}}}), nlohmann::json::array({{{"port", 1}}}),
              nlohmann::json::array({{{"host", "h"}, {"port", 1}, {"unknown", true}}})}))
    {
        auto entry = base;
        entry["upstreams"] = pool;
        reject(nlohmann::json::array({entry}).dump());
    }
    for (const char* field : {"host", "port", "timeout_ms"})
    {
        for (const auto& value : nlohmann::json::array(
                 {nullptr, true, 1.0, "private-value/path", -1, 0, 65536, 18446744073709551615ULL}))
        {
            auto entry = base;
            entry["upstreams"][0][field] = value;
            reject(nlohmann::json::array({entry}).dump());
        }
    }
    for (
        const char* raw :
        {R"([{"prefix":"/a","upstreams":[{"host":"h","host":"x","port":1}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","port":1,"port":2}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","port":1,"timeout_ms":1,"timeout_ms":2}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","\u0068ost":"x","port":1}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","port":1}],"upstreams":[{"host":"x","port":2}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":{"nested":1},"port":1}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","port":[1]}]}])",
         R"([{"prefix":"/a","upstreams":[{"host":"h","port":1}],"prefix":"/b"}])"})
    {
        reject(raw);
    }
}

TEST_F(ConfigTest, AcceptsOnlyBooleanStripPrefixAndRejectsDuplicates)
{
    for (const bool strip : {false, true})
    {
        const auto input =
            nlohmann::json::array(
                {{{"prefix", "/users"}, {"host", "h"}, {"port", 80}, {"strip_prefix", strip}}})
                .dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        const auto config = apigate::load_config_from_environment();
        ASSERT_EQ(config.routes.size(), 1U);
        EXPECT_EQ(config.routes.front().strip_prefix, strip);
    }
    for (const auto& value : nlohmann::json::array({0, 1, -1, 1.0, "true", "false", nullptr}))
    {
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

TEST_F(ConfigTest, ParsesRateLimitsAndKeepsNestedObjectsOutOfRouteCount)
{
    auto entries = nlohmann::json::array();
    for (int index = 0; index < 32; ++index)
    {
        entries.push_back({{"prefix", "/p" + std::to_string(index)},
                           {"host", "h"},
                           {"port", 80},
                           {"rate_limit",
                            {{"requests_per_second", index == 0 ? 1 : 1000000},
                             {"burst", index == 0 ? 1000000 : 1}}}});
    }
    const auto raw = entries.dump();
    ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
    const auto config = apigate::load_config_from_environment();
    ASSERT_EQ(config.routes.size(), 32U);
    const auto first = config.routes.front().rate_limit.value_or(apigate::RateLimitConfig{});
    const auto last = config.routes.back().rate_limit.value_or(apigate::RateLimitConfig{});
    EXPECT_EQ(first.requests_per_second, 1U);
    EXPECT_EQ(first.burst, 1000000U);
    EXPECT_EQ(last.requests_per_second, 1000000U);
    EXPECT_EQ(last.burst, 1U);
}

TEST_F(ConfigTest, RejectsInvalidRateLimitShapesFieldsAndNumbersWithoutEchoingValues)
{
    const auto base =
        nlohmann::json{{"prefix", "/private-prefix"}, {"host", "private-host.test"}, {"port", 80}};
    const auto check_rejected = [](const std::string& raw)
    {
        ASSERT_EQ(setenv("APIGATE_ROUTES", raw.c_str(), 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid rate limit was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string{error.what()}.find("private-"), std::string::npos);
            EXPECT_EQ(std::string{error.what()}.find(raw), std::string::npos);
        }
    };
    for (const auto& value :
         nlohmann::json::array({nullptr,
                                true,
                                1,
                                1.0,
                                "private-value",
                                nlohmann::json::array(),
                                nlohmann::json::object(),
                                {{"burst", 1}},
                                {{"requests_per_second", 1}},
                                {{"requests_per_second", 1}, {"burst", 1}, {"unknown", 1}}}))
    {
        auto entry = base;
        entry["rate_limit"] = value;
        check_rejected(nlohmann::json::array({entry}).dump());
    }
    for (const char* field : {"requests_per_second", "burst"})
    {
        for (const auto& invalid :
             nlohmann::json::array({0, -1, 1000001, true, false, 1.0, 1.5, "private-value", nullptr,
                                    std::uint64_t{18446744073709551615ULL}}))
        {
            auto entry = base;
            entry["rate_limit"] = {{"requests_per_second", 10}, {"burst", 20}};
            entry["rate_limit"][field] = invalid;
            check_rejected(nlohmann::json::array({entry}).dump());
        }
    }
    for (
        const char* raw :
        {R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":1,"burst":1,"burst":2}}])",
         R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":1,"requests_per_second":2,"burst":1}}])",
         R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":1,"burst":1,"\u0062urst":2}}])",
         R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":1,"burst":1},"rate_limit":{"requests_per_second":2,"burst":2}}])",
         R"([{"prefix":"/a","rate_limit":{"requests_per_second":1,"burst":1},"host":"h","host":"x","port":80}])",
         R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":{},"burst":1}}])",
         R"([{"prefix":"/a","host":"h","port":80,"rate_limit":{"requests_per_second":1,"burst":18446744073709551616}}])"})
    {
        check_rejected(raw);
    }
}

TEST_F(ConfigTest, RejectsRouteShapesFieldsDuplicatesAndTypes)
{
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
    for (const auto* input : invalid)
    {
        ASSERT_EQ(setenv("APIGATE_ROUTES", input, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
    for (const char* field : {"port", "timeout_ms"})
    {
        for (const auto& invalid_number :
             nlohmann::json::array({0, -1, 65536, 1.0, 1.5, "80", true, nullptr}))
        {
            auto entry = nlohmann::json{{"prefix", "/a"}, {"host", "h"}, {"port", 80}};
            entry[field] = invalid_number;
            const auto input = nlohmann::json::array({entry}).dump();
            ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
            EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                         apigate::ConfigError);
        }
    }
}

TEST_F(ConfigTest, RejectsUnsafeRoutePrefixes)
{
    const char* invalid[] = {"",        "users", "/a/",    "/a//b", "/a/./b",
                             "/a/../b", "/.",    "/..",    "/a?x",  "/a#x",
                             "/a b",    "/a\\b", "/a%20b", "/a\n",  "/caf\xc3\xa9"};
    for (const auto* prefix : invalid)
    {
        const auto input =
            nlohmann::json::array({{{"prefix", prefix}, {"host", "h"}, {"port", 80}}}).dump();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, EnforcesRouteCountAndRawByteBoundaries)
{
    auto entries = nlohmann::json::array();
    for (int index = 0; index < 32; ++index)
    {
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

TEST_F(ConfigTest, RejectsEveryLegacyConflictEvenEmptyValuesWithoutEchoingRoutes)
{
    const std::string input =
        R"([{"prefix":"/private-prefix","host":"private-host.test","port":80}])";
    for (const char* variable :
         {"APIGATE_UPSTREAM_HOST", "APIGATE_UPSTREAM_PORT", "APIGATE_UPSTREAM_TIMEOUT_MS"})
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_ROUTES", input.c_str(), 1), 0);
        ASSERT_EQ(setenv(variable, "", 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "conflicting configuration was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_STREQ(error.what(), "APIGATE_ROUTES conflicts with legacy upstream variables");
        }
    }
    clear_environment();
    for (const auto& invalid :
         {input.substr(0, input.size() - 1),
          std::string{R"([{"prefix":"/private-prefix?x","host":"private-host.test","port":80}])"},
          std::string{
              R"([{"prefix":"/private-prefix","host":"private-host.test/secret","port":80}])"}})
    {
        ASSERT_EQ(setenv("APIGATE_ROUTES", invalid.c_str(), 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid private configuration was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            EXPECT_EQ(std::string{error.what()}.find("private-"), std::string::npos);
        }
    }
}

TEST_F(ConfigTest, AcceptsInFlightByteBudgetBoundariesAndIntermediateValue)
{
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "1", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 1U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "123456789", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 123456789U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", "1073741824", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().max_in_flight_bytes, 1073741824U);
}

TEST_F(ConfigTest, RejectsInvalidInFlightByteBudgetWithoutEchoingValues)
{
    const char* invalid_values[] = {"",         "0",   "-1",  "1073741825",
                                    "12suffix", " 12", "12 ", "18446744073709551616"};
    for (const char* invalid_value : invalid_values)
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_MAX_IN_FLIGHT_BYTES", invalid_value, 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid in-flight byte budget was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            if (*invalid_value == '\0')
            {
                EXPECT_STREQ(error.what(), "APIGATE_MAX_IN_FLIGHT_BYTES must not be empty");
            }
            else
            {
                EXPECT_STREQ(error.what(),
                             "APIGATE_MAX_IN_FLIGHT_BYTES must be an integer from 1 to "
                             "1073741824");
            }
            if (std::string_view{invalid_value}.size() > 1)
            {
                EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
            }
        }
    }
}

TEST_F(ConfigTest, AcceptsShutdownGraceBoundaries)
{
    ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", "1", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().shutdown_grace_ms, 1U);

    clear_environment();
    ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", "60000", 1), 0);
    EXPECT_EQ(apigate::load_config_from_environment().shutdown_grace_ms, 60000U);
}

TEST_F(ConfigTest, RejectsInvalidShutdownGraceWithoutEchoingValues)
{
    const char* invalid_values[] = {"0", "-1", "60001", "12suffix", " 12", "12 ", ""};
    for (const char* invalid_value : invalid_values)
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_SHUTDOWN_GRACE_MS", invalid_value, 1), 0);
        try
        {
            static_cast<void>(apigate::load_config_from_environment());
            FAIL() << "invalid shutdown grace was accepted";
        }
        catch (const apigate::ConfigError& error)
        {
            if (*invalid_value == '\0')
            {
                EXPECT_STREQ(error.what(), "APIGATE_SHUTDOWN_GRACE_MS must not be empty");
            }
            else
            {
                EXPECT_STREQ(error.what(),
                             "APIGATE_SHUTDOWN_GRACE_MS must be an integer from 1 to 60000");
            }
            if (std::string_view{invalid_value}.size() > 1)
            {
                EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
            }
        }
    }
}

TEST_F(ConfigTest, AcceptsCapacityLimitBoundaries)
{
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

TEST_F(ConfigTest, RejectsInvalidCapacityLimitsWithoutEchoingValues)
{
    const char* variable_names[] = {
        "APIGATE_MAX_CONNECTIONS",
        "APIGATE_MAX_CONCURRENT_PROXIES",
    };
    const char* invalid_values[] = {"0", "-1", "65536", "12suffix", ""};

    for (const char* variable_name : variable_names)
    {
        for (const char* invalid_value : invalid_values)
        {
            clear_environment();
            ASSERT_EQ(setenv(variable_name, invalid_value, 1), 0);
            try
            {
                static_cast<void>(apigate::load_config_from_environment());
                FAIL() << "invalid capacity limit was accepted";
            }
            catch (const apigate::ConfigError& error)
            {
                if (*invalid_value != '\0')
                {
                    EXPECT_EQ(std::string{error.what()}.find(invalid_value), std::string::npos);
                }
            }
        }
    }
}

TEST_F(ConfigTest, EnablesStaticUpstreamWithDefaultTimeout)
{
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8081", 1), 0);

    const auto config = apigate::load_config_from_environment();

    ASSERT_TRUE(config.upstream.has_value());
    const auto upstream = config.upstream.value_or(apigate::UpstreamConfig{});
    EXPECT_EQ(upstream.host, "upstream.internal");
    EXPECT_EQ(upstream.port, 8081);
    EXPECT_EQ(upstream.timeout_ms, 3000U);
}

TEST_F(ConfigTest, AcceptsIpLiteralsAndCustomTimeout)
{
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

TEST_F(ConfigTest, RejectsIncompleteUpstreamConfiguration)
{
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    clear_environment();

    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
    clear_environment();

    ASSERT_EQ(setenv("APIGATE_UPSTREAM_TIMEOUT_MS", "100", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsEmptyOrUnsafeUpstreamHost)
{
    const char* invalid_hosts[] = {"",          "host name",  "http://upstream",
                                   "host/path", "host?query", "host#fragment",
                                   "user@host", "-bad.host",  "bad-.host"};
    for (const char* host : invalid_hosts)
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", host, 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, RejectsInvalidUpstreamPort)
{
    const char* invalid_ports[] = {"0", "65536", "-1", "80suffix"};
    for (const char* port : invalid_ports)
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", port, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, RejectsInvalidUpstreamTimeout)
{
    const char* invalid_timeouts[] = {"0", "60001", "-1", "100suffix"};
    for (const char* timeout : invalid_timeouts)
    {
        clear_environment();
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", "upstream.internal", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);
        ASSERT_EQ(setenv("APIGATE_UPSTREAM_TIMEOUT_MS", timeout, 1), 0);
        EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()),
                     apigate::ConfigError);
    }
}

TEST_F(ConfigTest, DoesNotEchoInvalidUpstreamValue)
{
    const std::string private_marker = "private-upstream-marker";
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_HOST", (private_marker + "/path").c_str(), 1), 0);
    ASSERT_EQ(setenv("APIGATE_UPSTREAM_PORT", "8080", 1), 0);

    try
    {
        static_cast<void>(apigate::load_config_from_environment());
        FAIL() << "invalid upstream host was accepted";
    }
    catch (const apigate::ConfigError& error)
    {
        EXPECT_EQ(std::string{error.what()}.find(private_marker), std::string::npos);
    }
}

TEST_F(ConfigTest, ReadsValidOverrides)
{
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

TEST_F(ConfigTest, AcceptsMaximumLengthLabel)
{
    const std::string service_name(64, 'a');
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", service_name.c_str(), 1), 0);

    const auto config = apigate::load_config_from_environment();

    EXPECT_EQ(config.service_name, service_name);
}

TEST_F(ConfigTest, RejectsLabelLongerThanMaximum)
{
    const std::string service_name(65, 'a');
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", service_name.c_str(), 1), 0);

    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsUnknownLogLevel)
{
    ASSERT_EQ(setenv("APIGATE_LOG_LEVEL", "verbose", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsUnsafeServiceName)
{
    ASSERT_EQ(setenv("APIGATE_SERVICE_NAME", "gateway with spaces", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsEmptyValue)
{
    ASSERT_EQ(setenv("APIGATE_ENVIRONMENT", "", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsInvalidListenAddress)
{
    ASSERT_EQ(setenv("APIGATE_LISTEN_ADDRESS", "localhost", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}

TEST_F(ConfigTest, RejectsInvalidListenPort)
{
    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "65536", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);

    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "80suffix", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);

    ASSERT_EQ(setenv("APIGATE_LISTEN_PORT", "-1", 1), 0);
    EXPECT_THROW(static_cast<void>(apigate::load_config_from_environment()), apigate::ConfigError);
}
// NOLINTEND(concurrency-mt-unsafe)

}  // namespace
