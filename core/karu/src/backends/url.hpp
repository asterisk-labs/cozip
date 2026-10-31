#ifndef KARU_BACKENDS_URL_HPP
#define KARU_BACKENDS_URL_HPP

#include "../request.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace karu::backends {

struct UrlParts {
    std::string scheme;
    std::string authority;
    std::string path;
    std::string query;
};

[[nodiscard]] std::string trim(std::string value);
[[nodiscard]] std::string lower(std::string value);
[[nodiscard]] std::string without_trailing_slash(std::string value);
[[nodiscard]] bool starts_with_scheme(std::string_view endpoint);
[[nodiscard]] std::expected<std::string, RequestError>
http_endpoint(std::string endpoint, std::string_view option, bool use_https = true);
[[nodiscard]] std::expected<UrlParts, RequestError> split_url(std::string_view url);
[[nodiscard]] std::string append_object(const std::string& endpoint, std::string_view container,
                                        std::string_view key);
[[nodiscard]] std::string form_encode(std::string_view text);

} // namespace karu::backends

#endif
