#include "credentials.hpp"

#include "../platform.hpp"
#include "../text.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <unordered_map>

namespace karu::backends {

std::expected<bool, RequestError> path_exists(const std::string& path, std::string_view purpose) {
    std::error_code error;
    const bool exists = std::filesystem::exists(os::path_from_utf8(path), error);
    if (error) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, std::string(purpose) +
                                                                      ": cannot inspect '" + path +
                                                                      "': " + error.message()});
    }
    return exists;
}

std::expected<std::string, RequestError> read_text_file(const std::string& path,
                                                        std::string_view purpose) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS,
                                            std::string(purpose) + ": cannot open '" + path + "'"});
    }
    constexpr std::size_t limit = 1u << 20;
    std::string contents;
    std::array<char, 4096> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::size_t read = static_cast<std::size_t>(stream.gcount());
        if (read > limit - contents.size()) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS, std::string(purpose) + ": file exceeds 1 MiB"});
        }
        contents.append(buffer.data(), read);
    }
    if (!stream.eof()) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS,
                                            std::string(purpose) + ": cannot read '" + path + "'"});
    }
    return contents;
}

std::optional<std::string> json_string(std::string_view json, std::string_view name) {
    const std::string needle = "\"" + std::string(name) + "\"";
    std::size_t position = 0;
    while ((position = json.find(needle, position)) != std::string_view::npos) {
        std::size_t cursor = position + needle.size();
        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])))
            ++cursor;
        if (cursor >= json.size() || json[cursor] != ':') {
            position += needle.size();
            continue;
        }
        ++cursor;
        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])))
            ++cursor;
        if (cursor >= json.size() || json[cursor] != '"')
            return std::nullopt;
        ++cursor;
        std::string result;
        while (cursor < json.size()) {
            const char c = json[cursor++];
            if (c == '"')
                return result;
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (cursor >= json.size())
                return std::nullopt;
            const char escaped = json[cursor++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::int64_t> json_integer(std::string_view json, std::string_view name) {
    if (auto quoted = json_string(json, name)) {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(quoted->data(), quoted->data() + quoted->size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == quoted->data() + quoted->size())
            return value;
    }
    const std::string needle = "\"" + std::string(name) + "\"";
    const std::size_t found = json.find(needle);
    if (found == std::string_view::npos)
        return std::nullopt;
    std::size_t cursor = json.find(':', found + needle.size());
    if (cursor == std::string_view::npos)
        return std::nullopt;
    ++cursor;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])))
        ++cursor;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(json.data() + cursor, json.data() + json.size(), value);
    return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
}

namespace {

std::size_t collect_body(char* data, std::size_t size, std::size_t count,
                         void* user_data) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return 0;
    const std::size_t length = size * count;
    constexpr std::size_t limit = 1u << 20;
    auto& result = *static_cast<std::string*>(user_data);
    if (length > limit - std::min(result.size(), limit))
        return 0;
    try {
        result.append(data, length);
    } catch (...) {
        return 0;
    }
    return length;
}

long curl_http_version(HttpVersion version) noexcept {
    switch (version) {
    case HttpVersion::Automatic:
        return CURL_HTTP_VERSION_NONE;
    case HttpVersion::Http2Tls:
        return CURL_HTTP_VERSION_2TLS;
    case HttpVersion::Http2PriorKnowledge:
        return CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;
    case HttpVersion::Http1_1:
        return CURL_HTTP_VERSION_1_1;
    }
    return CURL_HTTP_VERSION_1_1;
}

template <typename Value>
std::expected<void, RequestError> set_credential_option(CURL* easy, CURLoption option,
                                                        Value value) {
    const CURLcode code = curl_easy_setopt(easy, option, value);
    if (code == CURLE_OK)
        return {};
    return std::unexpected(
        RequestError{KARU_ERR_CREDENTIALS,
                     std::string("credential endpoint setup: ") + curl_easy_strerror(code)});
}

template <typename Value, typename... Rest>
std::expected<void, RequestError> set_credential_options(CURL* easy, CURLoption option, Value value,
                                                         Rest... rest) {
    if (auto configured = set_credential_option(easy, option, value); !configured)
        return configured;
    if constexpr (sizeof...(rest) == 0)
        return {};
    else
        return set_credential_options(easy, rest...);
}

} // namespace

std::expected<HttpResponse, RequestError>
credential_request(std::string_view method, const std::string& url, std::string_view body,
                   const std::vector<Header>& headers, long timeout_seconds,
                   const HttpRequestOptions& options) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    if (!easy)
        return std::unexpected(RequestError{KARU_ERR_NOMEM, "curl_easy_init failed"});
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> list(nullptr, curl_slist_free_all);
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS, "credential header is invalid"});
        }
        const std::string line = name + ": " + value;
        curl_slist* appended = curl_slist_append(list.get(), line.c_str());
        if (appended == nullptr)
            return std::unexpected(RequestError{KARU_ERR_NOMEM, "cannot build credential headers"});
        static_cast<void>(list.release());
        list.reset(appended);
    }
    HttpResponse response;
    std::array<char, CURL_ERROR_SIZE> error{};
    auto configured = set_credential_options(
        easy.get(), CURLOPT_URL, url.c_str(), CURLOPT_NOSIGNAL, 1L, CURLOPT_CONNECTTIMEOUT,
        timeout_seconds, CURLOPT_TIMEOUT, timeout_seconds * 2, CURLOPT_WRITEFUNCTION, collect_body,
        CURLOPT_WRITEDATA, &response.body, CURLOPT_ERRORBUFFER, error.data(), CURLOPT_HTTP_VERSION,
        curl_http_version(options.version));
    if (!configured)
        return std::unexpected(configured.error());
    if (!options.ca_bundle.empty()) {
        configured = set_credential_option(easy.get(), CURLOPT_CAINFO, options.ca_bundle.c_str());
        if (!configured)
            return std::unexpected(configured.error());
    }
    if (!options.ca_path.empty()) {
        configured = set_credential_option(easy.get(), CURLOPT_CAPATH, options.ca_path.c_str());
        if (!configured)
            return std::unexpected(configured.error());
    }
    if (!options.proxy.empty()) {
        configured = set_credential_option(easy.get(), CURLOPT_PROXY, options.proxy.c_str());
        if (!configured)
            return std::unexpected(configured.error());
    }
    if (!options.proxy_user_password.empty()) {
        configured = set_credential_option(easy.get(), CURLOPT_PROXYUSERPWD,
                                           options.proxy_user_password.c_str());
        if (!configured)
            return std::unexpected(configured.error());
    }
    if (!options.user_agent.empty()) {
        configured =
            set_credential_option(easy.get(), CURLOPT_USERAGENT, options.user_agent.c_str());
        if (!configured)
            return std::unexpected(configured.error());
    }
#if LIBCURL_VERSION_NUM >= 0x075500
    configured = set_credential_option(easy.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#else
    configured =
        set_credential_option(easy.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    if (!configured)
        return std::unexpected(configured.error());
    if (list) {
        configured = set_credential_option(easy.get(), CURLOPT_HTTPHEADER, list.get());
        if (!configured)
            return std::unexpected(configured.error());
    }

    if (body.size() > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "credential request body is too large"});
    }
    std::string stable_method;
    if (method == "POST") {
        configured =
            set_credential_options(easy.get(), CURLOPT_POST, 1L, CURLOPT_POSTFIELDS, body.data(),
                                   CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        stable_method.assign(method);
        configured =
            set_credential_option(easy.get(), CURLOPT_CUSTOMREQUEST, stable_method.c_str());
        if (configured && !body.empty()) {
            configured =
                set_credential_options(easy.get(), CURLOPT_POSTFIELDS, body.data(),
                                       CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }
    if (!configured)
        return std::unexpected(configured.error());

    const CURLcode code = curl_easy_perform(easy.get());
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &response.status);
    if (code != CURLE_OK) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         std::string("credential endpoint: ") +
                             (error[0] == '\0' ? curl_easy_strerror(code) : error.data())});
    }
    return response;
}

std::expected<ProviderCredentials, RequestError>
oauth_token(const std::string& url, const std::string& form,
            const std::vector<Header>& extra_headers, const HttpRequestOptions& options) {
    std::vector<Header> headers = extra_headers;
    headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    auto response = credential_request("POST", url, form, headers, 5, options);
    if (!response)
        return std::unexpected(response.error());
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, concat("credential endpoint returned HTTP ", response->status)});
    }
    auto token = json_string(response->body, "access_token");
    if (!token || token->empty())
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "credential response has no access_token"});
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t lifetime = json_integer(response->body, "expires_in").value_or(3600);
    ProviderCredentials result;
    result.bearer_token = std::move(*token);
    result.expires_at = now + std::max<std::int64_t>(lifetime, 60);
    return result;
}

std::string date_utc(std::time_t now, const char* format) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::array<char, 64> result{};
    std::strftime(result.data(), result.size(), format, &utc);
    return result.data();
}

std::string rfc7231_date(std::time_t now) {
    return date_utc(now, "%a, %d %b %Y %H:%M:%S GMT");
}

std::int64_t iso8601_epoch(std::string_view text) {
    std::tm utc{};
    std::istringstream stream(std::string(text.substr(0, 19)));
    stream >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail())
        return 0;
    return static_cast<std::int64_t>(os::timegm(utc));
}

std::expected<IniFile, RequestError> read_ini(const std::string& path, std::string_view purpose) {
    auto contents = read_text_file(path, purpose);
    if (!contents)
        return std::unexpected(contents.error());
    IniFile result;
    std::string section;
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.starts_with('#') || line.starts_with(';'))
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t separator = line.find_first_of("=:");
        if (separator == std::string::npos || section.empty())
            continue;
        result[section][lower(trim(line.substr(0, separator)))] = trim(line.substr(separator + 1));
    }
    return result;
}

std::string range_header(std::uint64_t first, std::uint64_t length) {
    return concat("bytes=", first, "-", first + length - 1);
}

std::string suffix_range_header(std::uint64_t length) {
    return concat("bytes=-", length);
}

std::expected<ProviderCredentials, RequestError>
copy_callback_credentials(const std::shared_ptr<CredentialCallback>& callback,
                          karu_credentials_kind kind, std::string_view path) {
    karu_credentials raw{};
    const std::string stable_path(path);
    const karu_status status =
        callback->function(callback->user_data, kind, stable_path.c_str(), &raw);
    if (status != KARU_OK) {
        return std::unexpected(RequestError{
            status < 0 ? status : KARU_ERR_CREDENTIALS,
            concat(path, ": custom credential provider returned ", karu_status_string(status))});
    }
    const auto copy = [](const char* value) { return value == nullptr ? "" : value; };
    return ProviderCredentials{copy(raw.access_key_id),
                               copy(raw.secret_access_key),
                               copy(raw.session_token),
                               copy(raw.bearer_token),
                               copy(raw.sas_token),
                               copy(raw.account_name),
                               {},
                               copy(raw.cache_prefix),
                               raw.expires_at};
}

std::expected<void, RequestError> add_configured_headers(const ConfigSnapshot& config,
                                                         std::string_view path,
                                                         std::vector<Header>& headers,
                                                         bool protect_authentication) {
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            return std::unexpected(
                RequestError{KARU_ERR_CONFIG, "a generated HTTP header is invalid"});
        }
    }
    static_cast<void>(path);
    for (const ConfiguredHeader& configured : config.configured_headers()) {
        if (configured.malformed) {
            return std::unexpected(
                RequestError{KARU_ERR_CONFIG, "KARU_HTTP_HEADERS contains a malformed header"});
        }
        const std::string& normalized = configured.lowercase_name;
        const bool duplicate = std::ranges::any_of(
            headers, [&](const Header& current) { return lower(current.first) == normalized; });
        const bool protected_header =
            protect_authentication &&
            (normalized == "authorization" || normalized == "date" || normalized == "if-match" ||
             normalized.starts_with("x-amz-") || normalized.starts_with("x-goog-") ||
             normalized.starts_with("x-ms-"));
        if (normalized == "host" || normalized == "range" || duplicate || protected_header) {
            return std::unexpected(RequestError{
                KARU_ERR_CONFIG,
                "KARU_HTTP_HEADERS cannot override Karu-managed header '" + configured.name + "'"});
        }
        headers.emplace_back(configured.name, configured.value);
    }
    return {};
}

} // namespace karu::backends
