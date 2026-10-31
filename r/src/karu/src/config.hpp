#ifndef KARU_CONFIG_HPP
#define KARU_CONFIG_HPP

#include "karu/karu.h"
#include "request.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace karu {

using OptionMap = std::unordered_map<std::string, std::string>;

struct CredentialCallback {
    CredentialCallback(karu_credentials_provider callback, void* data,
                       karu_credentials_provider_free destroy) noexcept
        : function(callback), user_data(data), release(destroy) {}
    CredentialCallback(const CredentialCallback&) = delete;
    CredentialCallback& operator=(const CredentialCallback&) = delete;

    karu_credentials_provider function = nullptr;
    void* user_data = nullptr;
    karu_credentials_provider_free release = nullptr;

    ~CredentialCallback();
};

// A parsed KARU_HTTP_HEADERS entry, or the error that stopped parsing.
struct ConfiguredHeader {
    std::string name;
    std::string lowercase_name;
    std::string value;
    bool malformed = false;

    bool operator==(const ConfiguredHeader&) const = default;
};

struct PathOptions {
    std::string prefix;
    OptionMap values;

    bool operator==(const PathOptions&) const = default;
};

struct ClientOptions {
    int concurrency = 64;
    int io_threads = 4;
    std::uint64_t coalesce_gap = 1u << 20;
    std::uint64_t coalesce_limit = 64u << 20;
    std::size_t coalesce_parts = 1024;
    std::uint64_t coalesce_amplification = 16;
    std::uint64_t range_fallback_limit = 8u << 20;
    int max_attempts = 8;
    long request_timeout_seconds = 120;
    long connect_timeout_seconds = 30;
    long low_speed_time_seconds = 60;
    long low_speed_limit = 1024;

    bool operator==(const ClientOptions&) const = default;
};

class ConfigSnapshot;

class ConfigBuilder {
  public:
    explicit ConfigBuilder(bool include_environment);

    [[nodiscard]] std::expected<void, std::string> set(std::string_view name, const char* value);
    [[nodiscard]] std::expected<void, std::string>
    set_path(std::string_view prefix, std::string_view name, const char* value);
    [[nodiscard]] std::expected<void, std::string>
    set_provider(karu_credentials_kind kind, karu_credentials_provider provider, void* user_data,
                 karu_credentials_provider_free release);

    [[nodiscard]] std::expected<ConfigSnapshot, std::string> freeze() const;

  private:
    OptionMap environment_;
    OptionMap explicit_;
    std::vector<PathOptions> paths_;
    std::unordered_map<int, std::shared_ptr<CredentialCallback>> providers_;
    std::string home_directory_;
    std::string cache_directory_;
    std::string app_data_directory_;
    std::string system_ca_bundle_;
    bool discover_default_credentials_ = false;

    friend class ConfigSnapshot;
};

class ConfigSnapshot {
  public:
    ConfigSnapshot() = default;

    [[nodiscard]] std::string option(std::string_view path, std::string_view name,
                                     std::string_view fallback = {}) const;
    [[nodiscard]] bool has_option(std::string_view path, std::string_view name) const;
    [[nodiscard]] std::shared_ptr<CredentialCallback> provider(karu_credentials_kind kind) const;
    [[nodiscard]] std::string scope_key(std::string_view path,
                                        std::span<const std::string_view> names) const;
    [[nodiscard]] std::string expand_user_path(std::string_view path) const;
    [[nodiscard]] std::string default_aws_path(std::string_view suffix) const;
    [[nodiscard]] std::string default_gcloud_adc_path() const;
    [[nodiscard]] std::string hugging_face_token_path(std::string_view path) const;
    [[nodiscard]] bool discover_default_credentials() const noexcept {
        return discover_default_credentials_;
    }
    // HTTP options are client-wide and computed by freeze(); path is unused.
    [[nodiscard]] HttpRequestOptions http_options(std::string_view path = {}) const {
        static_cast<void>(path);
        return http_;
    }
    [[nodiscard]] const std::vector<ConfiguredHeader>& configured_headers() const noexcept {
        return configured_headers_;
    }
    [[nodiscard]] bool has_configured_headers() const noexcept { return has_configured_headers_; }
    [[nodiscard]] const ClientOptions& client_options() const noexcept { return client_; }
    [[nodiscard]] bool operator==(const ConfigSnapshot&) const noexcept = default;

  private:
    OptionMap environment_;
    OptionMap explicit_;
    std::vector<PathOptions> paths_;
    std::unordered_map<int, std::shared_ptr<CredentialCallback>> providers_;
    std::string home_directory_;
    std::string cache_directory_;
    std::string app_data_directory_;
    std::string system_ca_bundle_;
    bool discover_default_credentials_ = false;
    ClientOptions client_;
    HttpRequestOptions http_ = default_http_options();
    std::vector<ConfiguredHeader> configured_headers_;
    bool has_configured_headers_ = false;

    [[nodiscard]] static HttpRequestOptions default_http_options();
    [[nodiscard]] HttpRequestOptions compute_http_options() const;

    friend class ConfigBuilder;
};

[[nodiscard]] std::string canonical_option_name(std::string_view name);
[[nodiscard]] std::string canonical_vsi_path(std::string_view path);
[[nodiscard]] bool path_prefix_matches(std::string_view path, std::string_view prefix) noexcept;
[[nodiscard]] bool option_is_true(std::string_view value) noexcept;

} // namespace karu

struct karu_config {
    explicit karu_config(bool include_environment) : builder(include_environment) {}
    karu::ConfigBuilder builder;
};

#endif
