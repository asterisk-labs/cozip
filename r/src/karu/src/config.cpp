#include "config.hpp"

#include "config_options.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace karu {
namespace {

std::string uppercase(std::string_view input) {
    std::string result(input);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

template <typename Integer>
std::expected<Integer, std::string> parse_integer(std::string_view name, std::string_view text,
                                                  Integer minimum, Integer maximum) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum ||
        value > maximum) {
        return std::unexpected(std::string(name) + " must be an integer in [" +
                               std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    }
    return value;
}

std::string trim_text(std::string_view value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && whitespace(static_cast<unsigned char>(value[first])))
        ++first;
    while (last > first && whitespace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return std::string(value.substr(first, last - first));
}

std::string lower_text(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Defer malformed-header errors to request construction.
std::vector<ConfiguredHeader> parse_configured_headers(std::string_view text) {
    std::vector<ConfiguredHeader> result;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find_first_of("\r\n", start);
        const std::string line = trim_text(text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                result.push_back(ConfiguredHeader{
                    .name = {}, .lowercase_name = {}, .value = {}, .malformed = true});
                break;
            }
            std::string name = trim_text(std::string_view(line).substr(0, colon));
            std::string lowercase_name = lower_text(name);
            result.push_back(
                ConfiguredHeader{.name = std::move(name),
                                 .lowercase_name = std::move(lowercase_name),
                                 .value = trim_text(std::string_view(line).substr(colon + 1)),
                                 .malformed = false});
        }
        if (end == std::string_view::npos)
            break;
        start = text.find_first_not_of("\r\n", end);
        if (start == std::string_view::npos)
            break;
    }
    return result;
}

const std::string* find(const OptionMap& values, const std::string& name) {
    const auto iterator = values.find(name);
    return iterator == values.end() ? nullptr : &iterator->second;
}

bool known_option(std::string_view canonical) {
    bool known = false;
    config_options::for_each_environment([&](const char* candidate) {
        known = known || canonical_option_name(candidate) == canonical;
    });
    return known;
}

bool valid_boolean(std::string_view value) {
    const std::string normalized = uppercase(value);
    return normalized == "YES" || normalized == "NO" || normalized == "TRUE" ||
           normalized == "FALSE" || normalized == "ON" || normalized == "OFF" ||
           normalized == "1" || normalized == "0";
}

bool valid_http_version(std::string_view value) {
    const std::string normalized = uppercase(value);
    return normalized == "AUTO" || normalized == "1.1" || normalized == "2" ||
           normalized == "2TLS" || normalized == "2PRIOR_KNOWLEDGE";
}

std::string system_ca_bundle() {
#if defined(__linux__)
    static constexpr std::array candidates{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/tls/cacert.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
#elif defined(__APPLE__)
    static constexpr std::array candidates{
        "/etc/ssl/cert.pem",
    };
#endif
#if defined(__linux__) || defined(__APPLE__)
    for (const char* path : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            return path;
    }
#endif
    return {};
}

std::string append_local_path(std::string base, std::string_view suffix) {
    if (!base.empty() && base.back() != '/' && base.back() != '\\')
        base.push_back('/');
    base.append(suffix);
    return base;
}

std::expected<void, std::string> validate_values(const OptionMap& values) {
    static constexpr std::array BOOLEAN_OPTIONS{
        "AWS_NO_SIGN_REQUEST",   "AWS_HTTPS",
        "AWS_VIRTUAL_HOSTING",   "AWS_EC2_METADATA_DISABLED",
        "GCS_NO_SIGN_REQUEST",   "GCS_METADATA_DISABLED",
        "AZURE_NO_SIGN_REQUEST", "SOURCE_NO_SIGN_REQUEST",
    };
    for (const char* name : BOOLEAN_OPTIONS) {
        if (const std::string* value = find(values, name);
            value != nullptr && !valid_boolean(*value)) {
            return std::unexpected(std::string(name) +
                                   " must be YES/NO, TRUE/FALSE, ON/OFF, or 1/0");
        }
    }
    if (const std::string* value = find(values, "AWS_METADATA_SERVICE_TIMEOUT"); value != nullptr) {
        if (auto parsed = parse_integer<long>("AWS_METADATA_SERVICE_TIMEOUT", *value, 1, 60);
            !parsed) {
            return std::unexpected(parsed.error());
        }
    }
    if (const std::string* value = find(values, "AWS_REQUEST_PAYER");
        value != nullptr && uppercase(*value) != "REQUESTER") {
        return std::unexpected(std::string("AWS_REQUEST_PAYER must be 'requester'"));
    }
    if (const std::string* value = find(values, "KARU_HTTP_VERSION");
        value != nullptr && !valid_http_version(*value)) {
        return std::unexpected("KARU_HTTP_VERSION must be AUTO, 1.1, 2TLS, 2, or "
                               "2PRIOR_KNOWLEDGE");
    }
    return {};
}

} // namespace

CredentialCallback::~CredentialCallback() {
    if (release != nullptr)
        release(user_data);
}

std::string canonical_option_name(std::string_view name) {
    std::string result = uppercase(name);
    // Provider-specific endpoint wins over a general SDK endpoint, but all
    // explicit spellings target the same Karu option. This prevents an env
    // alias from silently overriding a value set through the API.
    if (result == "AWS_ENDPOINT_URL" || result == "AWS_ENDPOINT_URL_S3")
        return "AWS_S3_ENDPOINT";
    if (result == "AWS_DEFAULT_PROFILE")
        return "AWS_PROFILE";
    if (result == "AWS_DEFAULT_REGION")
        return "AWS_REGION";
    if (result == "KARU_MAX_RETRIES")
        return "KARU_MAX_ATTEMPTS";
    if (result == "HUGGING_FACE_HUB_TOKEN")
        return "HF_TOKEN";
    if (result == "CURL_CA_BUNDLE" || result == "SSL_CERT_FILE")
        return "KARU_HTTP_CA_BUNDLE";
    if (result == "SOURCE_PROXY_URL")
        return "SOURCE_ENDPOINT";
    return result;
}

std::string canonical_vsi_path(std::string_view path) {
    auto convert = [&](std::string_view prefix, std::string_view vsi) {
        return std::string(vsi) + std::string(path.substr(prefix.size()));
    };
    if (path.starts_with("s3://"))
        return convert("s3://", "/vsis3/");
    if (path.starts_with("gs://"))
        return convert("gs://", "/vsigs/");
    if (path.starts_with("az://"))
        return convert("az://", "/vsiaz/");
    if (path.starts_with("abfs://"))
        return convert("abfs://", "/vsiadls/");
    if (path.starts_with("hf://"))
        return convert("hf://", "/vsihf/");
    if (path.starts_with("source://"))
        return convert("source://", "/vsisource/");
    return std::string(path);
}

bool path_prefix_matches(std::string_view path, std::string_view prefix) noexcept {
    if (!path.starts_with(prefix))
        return false;
    return path.size() == prefix.size() || prefix.ends_with('/') || path[prefix.size()] == '/';
}

bool option_is_true(std::string_view value) noexcept {
    auto equals = [&](std::string_view expected) {
        return value.size() == expected.size() &&
               std::ranges::equal(value, expected, [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left)) ==
                          std::tolower(static_cast<unsigned char>(right));
               });
    };
    return equals("yes") || equals("true") || equals("on") || value == "1";
}

ConfigBuilder::ConfigBuilder(bool include_environment)
    : discover_default_credentials_(include_environment) {
    if (!include_environment)
        return;
    system_ca_bundle_ = system_ca_bundle();
    // Later aliases in this table never override an earlier, more specific
    // spelling. Explicit builder values are held in a separate higher layer.
    config_options::for_each_environment([&](const char* name) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return;
        environment_.try_emplace(canonical_option_name(name), value);
    });
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && *cache != '\0')
        cache_directory_ = cache;
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr)
        home_directory_ = home;
    if (const char* app_data = std::getenv("APPDATA"); app_data != nullptr)
        app_data_directory_ = app_data;
#else
    if (const char* home = std::getenv("HOME"); home != nullptr)
        home_directory_ = home;
#endif
}

std::expected<void, std::string> ConfigBuilder::set(std::string_view name, const char* value) {
    if (name.empty())
        return std::unexpected("option name is empty");
    const std::string canonical = canonical_option_name(name);
    if (!known_option(canonical))
        return std::unexpected("unknown Karu option '" + std::string(name) + "'");
    if (value == nullptr) {
        explicit_.erase(canonical);
    } else {
        explicit_[canonical] = value;
    }
    return {};
}

std::expected<void, std::string> ConfigBuilder::set_path(std::string_view prefix,
                                                         std::string_view name, const char* value) {
    if (prefix.empty())
        return std::unexpected("path-specific option has an empty prefix");
    if (name.empty())
        return std::unexpected("option name is empty");
    const std::string canonical_prefix = canonical_vsi_path(prefix);
    const std::string canonical_name = canonical_option_name(name);
    if (!known_option(canonical_name))
        return std::unexpected("unknown Karu option '" + std::string(name) + "'");
    if (canonical_name.starts_with("KARU_"))
        return std::unexpected(canonical_name + " is client-wide and cannot be path-specific");

    auto rule = std::ranges::find(paths_, canonical_prefix, &PathOptions::prefix);
    if (rule == paths_.end()) {
        paths_.push_back(PathOptions{.prefix = canonical_prefix, .values = {}});
        rule = std::prev(paths_.end());
    }
    if (value == nullptr) {
        rule->values.erase(canonical_name);
    } else {
        rule->values[canonical_name] = value;
    }
    return {};
}

std::expected<void, std::string>
ConfigBuilder::set_provider(karu_credentials_kind kind, karu_credentials_provider provider,
                            void* user_data, karu_credentials_provider_free release) {
    if (kind < KARU_CREDENTIALS_AWS || kind > KARU_CREDENTIALS_SOURCE)
        return std::unexpected("unknown credentials kind");
    if (provider == nullptr) {
        if (user_data != nullptr || release != nullptr) {
            return std::unexpected(
                "clearing a credentials provider requires null user_data and release");
        }
        providers_.erase(static_cast<int>(kind));
        return {};
    }
    providers_[static_cast<int>(kind)] =
        std::make_shared<CredentialCallback>(provider, user_data, release);
    return {};
}

std::string ConfigSnapshot::option(std::string_view path, std::string_view name,
                                   std::string_view fallback) const {
    const std::string canonical_name = canonical_option_name(name);
    const std::string canonical_path = canonical_vsi_path(path);
    const PathOptions* best = nullptr;
    for (const PathOptions& candidate : paths_) {
        if (!path_prefix_matches(canonical_path, candidate.prefix) ||
            !candidate.values.contains(canonical_name)) {
            continue;
        }
        if (best == nullptr || candidate.prefix.size() > best->prefix.size()) {
            best = &candidate;
        }
    }
    if (best != nullptr)
        return best->values.at(canonical_name);
    if (const std::string* value = find(explicit_, canonical_name))
        return *value;
    if (const std::string* value = find(environment_, canonical_name))
        return *value;
    return std::string(fallback);
}

bool ConfigSnapshot::has_option(std::string_view path, std::string_view name) const {
    const std::string sentinel("\0missing", 8);
    return option(path, name, sentinel) != sentinel;
}

std::shared_ptr<CredentialCallback> ConfigSnapshot::provider(karu_credentials_kind kind) const {
    const auto iterator = providers_.find(static_cast<int>(kind));
    return iterator == providers_.end() ? nullptr : iterator->second;
}

std::string ConfigSnapshot::scope_key(std::string_view path,
                                      std::span<const std::string_view> names) const {
    // A client owns one immutable snapshot, so the supplying layer and path
    // prefix are enough to identify a credential scope without copying secrets.
    const std::string canonical_path = canonical_vsi_path(path);
    std::string result;
    for (std::string_view raw_name : names) {
        const std::string name = canonical_option_name(raw_name);
        const PathOptions* best = nullptr;
        for (const PathOptions& candidate : paths_) {
            if (!path_prefix_matches(canonical_path, candidate.prefix) ||
                !candidate.values.contains(name))
                continue;
            if (best == nullptr || candidate.prefix.size() > best->prefix.size())
                best = &candidate;
        }
        result += name;
        result.push_back('=');
        if (best != nullptr)
            result += best->prefix;
        else if (explicit_.contains(name))
            result += "explicit";
        else if (environment_.contains(name))
            result += "environment";
        else
            result += "default";
        result.push_back('\n');
    }
    return result;
}

std::string ConfigSnapshot::expand_user_path(std::string_view path) const {
    if (home_directory_.empty())
        return std::string(path);
    if (path == "~")
        return home_directory_;
    if (!path.starts_with("~/") && !path.starts_with("~\\"))
        return std::string(path);
    return home_directory_ + std::string(path.substr(1));
}

std::string ConfigSnapshot::default_aws_path(std::string_view suffix) const {
    if (home_directory_.empty())
        return {};
    return home_directory_ + "/.aws/" + std::string(suffix);
}

std::string ConfigSnapshot::default_gcloud_adc_path() const {
#ifdef _WIN32
    if (!app_data_directory_.empty())
        return app_data_directory_ + "/gcloud/application_default_credentials.json";
    if (home_directory_.empty())
        return {};
    return home_directory_ + "/AppData/Roaming/gcloud/application_default_credentials.json";
#else
    if (home_directory_.empty())
        return {};
    return home_directory_ + "/.config/gcloud/application_default_credentials.json";
#endif
}

std::string ConfigSnapshot::hugging_face_token_path(std::string_view path) const {
    if (std::string configured = option(path, "HF_TOKEN_PATH"); !configured.empty())
        return expand_user_path(configured);

    if (std::string home = option(path, "HF_HOME"); !home.empty())
        return append_local_path(expand_user_path(home), "token");

    if (!discover_default_credentials_)
        return {};
    if (!cache_directory_.empty())
        return append_local_path(expand_user_path(cache_directory_), "huggingface/token");
    if (home_directory_.empty())
        return {};
    return home_directory_ + "/.cache/huggingface/token";
}

HttpRequestOptions ConfigSnapshot::default_http_options() {
    HttpRequestOptions result;
    result.user_agent = "karu/" KARU_VERSION_STRING;
    return result;
}

HttpRequestOptions ConfigSnapshot::compute_http_options() const {
    const std::string_view path;
    HttpRequestOptions result;
    const std::string version = uppercase(option(path, "KARU_HTTP_VERSION", "1.1"));
    if (version == "AUTO")
        result.version = HttpVersion::Automatic;
    else if (version == "2" || version == "2TLS")
        result.version = HttpVersion::Http2Tls;
    else if (version == "2PRIOR_KNOWLEDGE")
        result.version = HttpVersion::Http2PriorKnowledge;
    result.ca_bundle = option(path, "KARU_HTTP_CA_BUNDLE");
    result.ca_path = option(path, "KARU_HTTP_CA_PATH");
    if (!has_option(path, "KARU_HTTP_CA_BUNDLE") && !has_option(path, "KARU_HTTP_CA_PATH")) {
        result.ca_bundle = system_ca_bundle_;
    }
    result.proxy = option(path, "KARU_HTTP_PROXY");
    result.proxy_user_password = option(path, "KARU_HTTP_PROXY_CREDENTIALS");
    result.user_agent = option(path, "KARU_HTTP_USER_AGENT", "karu/" KARU_VERSION_STRING);
    return result;
}

std::expected<ConfigSnapshot, std::string> ConfigBuilder::freeze() const {
    ConfigSnapshot result;
    result.environment_ = environment_;
    result.explicit_ = explicit_;
    result.paths_ = paths_;
    result.providers_ = providers_;
    result.home_directory_ = home_directory_;
    result.cache_directory_ = cache_directory_;
    result.app_data_directory_ = app_data_directory_;
    result.system_ca_bundle_ = system_ca_bundle_;
    result.discover_default_credentials_ = discover_default_credentials_;

    if (auto valid = validate_values(result.environment_); !valid)
        return std::unexpected(valid.error());
    if (auto valid = validate_values(result.explicit_); !valid)
        return std::unexpected(valid.error());
    for (const PathOptions& rule : result.paths_) {
        if (auto valid = validate_values(rule.values); !valid)
            return std::unexpected(rule.prefix + ": " + valid.error());
    }

    const auto global = [&](std::string_view name, std::string_view fallback) {
        return result.option({}, name, fallback);
    };
    auto concurrency =
        parse_integer<int>("KARU_CONCURRENCY", global("KARU_CONCURRENCY", "64"), 1, 4096);
    if (!concurrency)
        return std::unexpected(concurrency.error());
    auto io_threads = parse_integer<int>("KARU_IO_THREADS", global("KARU_IO_THREADS", "4"), 1, 64);
    if (!io_threads)
        return std::unexpected(io_threads.error());
    auto gap =
        parse_integer<std::uint64_t>("KARU_COALESCE_GAP", global("KARU_COALESCE_GAP", "1048576"), 0,
                                     std::numeric_limits<std::uint64_t>::max());
    if (!gap)
        return std::unexpected(gap.error());
    auto coalesce_limit = parse_integer<std::uint64_t>("KARU_COALESCE_LIMIT",
                                                       global("KARU_COALESCE_LIMIT", "67108864"), 1,
                                                       std::numeric_limits<std::uint64_t>::max());
    if (!coalesce_limit)
        return std::unexpected(coalesce_limit.error());
    auto coalesce_parts = parse_integer<std::size_t>(
        "KARU_COALESCE_PARTS", global("KARU_COALESCE_PARTS", "1024"), 1, 1048576);
    if (!coalesce_parts)
        return std::unexpected(coalesce_parts.error());
    auto coalesce_amplification = parse_integer<std::uint64_t>(
        "KARU_COALESCE_AMPLIFICATION", global("KARU_COALESCE_AMPLIFICATION", "16"), 1, 1048576);
    if (!coalesce_amplification)
        return std::unexpected(coalesce_amplification.error());
    auto range_fallback_limit = parse_integer<std::uint64_t>(
        "KARU_RANGE_FALLBACK_LIMIT", global("KARU_RANGE_FALLBACK_LIMIT", "8388608"), 0,
        std::numeric_limits<std::uint64_t>::max());
    if (!range_fallback_limit)
        return std::unexpected(range_fallback_limit.error());
    auto attempts =
        parse_integer<int>("KARU_MAX_ATTEMPTS", global("KARU_MAX_ATTEMPTS", "8"), 1, 16);
    if (!attempts)
        return std::unexpected(attempts.error());
    auto request_timeout = parse_integer<long>("KARU_REQUEST_TIMEOUT",
                                               global("KARU_REQUEST_TIMEOUT", "120"), 0, 86400);
    if (!request_timeout)
        return std::unexpected(request_timeout.error());
    auto connect =
        parse_integer<long>("KARU_CONNECT_TIMEOUT", global("KARU_CONNECT_TIMEOUT", "30"), 1, 3600);
    if (!connect)
        return std::unexpected(connect.error());
    auto low_time =
        parse_integer<long>("KARU_LOW_SPEED_TIME", global("KARU_LOW_SPEED_TIME", "60"), 0, 3600);
    if (!low_time)
        return std::unexpected(low_time.error());
    auto low_limit =
        parse_integer<long>("KARU_LOW_SPEED_LIMIT", global("KARU_LOW_SPEED_LIMIT", "1024"), 0,
                            std::numeric_limits<long>::max());
    if (!low_limit)
        return std::unexpected(low_limit.error());

    result.client_ = ClientOptions{.concurrency = *concurrency,
                                   .io_threads = *io_threads,
                                   .coalesce_gap = *gap,
                                   .coalesce_limit = *coalesce_limit,
                                   .coalesce_parts = *coalesce_parts,
                                   .coalesce_amplification = *coalesce_amplification,
                                   .range_fallback_limit = *range_fallback_limit,
                                   .max_attempts = *attempts,
                                   .request_timeout_seconds = *request_timeout,
                                   .connect_timeout_seconds = *connect,
                                   .low_speed_time_seconds = *low_time,
                                   .low_speed_limit = *low_limit};
    result.http_ = result.compute_http_options();
    const std::string headers = global("KARU_HTTP_HEADERS", {});
    result.has_configured_headers_ = !headers.empty();
    result.configured_headers_ = parse_configured_headers(headers);
    return result;
}

} // namespace karu
