#ifndef KARU_BACKENDS_CREDENTIALS_HPP
#define KARU_BACKENDS_CREDENTIALS_HPP

#include "../config.hpp"
#include "../request.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace karu::backends {

using Header = std::pair<std::string, std::string>;

struct HttpResponse {
    long status = 0;
    std::string body;
};

using IniSection = std::unordered_map<std::string, std::string>;
using IniFile = std::unordered_map<std::string, IniSection>;

[[nodiscard]] std::expected<std::string, RequestError> read_text_file(const std::string& path,
                                                                      std::string_view purpose);
[[nodiscard]] std::expected<bool, RequestError> path_exists(const std::string& path,
                                                            std::string_view purpose);
[[nodiscard]] std::optional<std::string> json_string(std::string_view json, std::string_view name);
[[nodiscard]] std::optional<std::int64_t> json_integer(std::string_view json,
                                                       std::string_view name);

[[nodiscard]] std::expected<HttpResponse, RequestError>
credential_request(std::string_view method, const std::string& url, std::string_view body,
                   const std::vector<Header>& headers, long timeout_seconds = 5,
                   const HttpRequestOptions& options = {});
[[nodiscard]] std::expected<ProviderCredentials, RequestError>
oauth_token(const std::string& url, const std::string& form,
            const std::vector<Header>& extra_headers = {}, const HttpRequestOptions& options = {});

[[nodiscard]] std::string date_utc(std::time_t now, const char* format);
[[nodiscard]] std::string rfc7231_date(std::time_t now);
[[nodiscard]] std::int64_t iso8601_epoch(std::string_view text);
[[nodiscard]] std::expected<IniFile, RequestError> read_ini(const std::string& path,
                                                            std::string_view purpose);
[[nodiscard]] std::string range_header(std::uint64_t first, std::uint64_t length);
[[nodiscard]] std::string suffix_range_header(std::uint64_t length);

[[nodiscard]] std::expected<ProviderCredentials, RequestError>
copy_callback_credentials(const std::shared_ptr<CredentialCallback>& callback,
                          karu_credentials_kind kind, std::string_view path);
[[nodiscard]] std::expected<void, RequestError> add_configured_headers(const ConfigSnapshot& config,
                                                                       std::string_view path,
                                                                       std::vector<Header>& headers,
                                                                       bool protect_authentication);

} // namespace karu::backends

#endif
