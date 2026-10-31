#include "url.hpp"

#include "../uri.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace karu::backends {

std::string trim(std::string value) {
    const auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::ranges::find_if_not(value, whitespace);
    const auto last = std::ranges::find_if_not(value | std::views::reverse, whitespace).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string without_trailing_slash(std::string value) {
    while (value.size() > 1 && value.ends_with('/'))
        value.pop_back();
    return value;
}

bool starts_with_scheme(std::string_view endpoint) {
    const std::string normalized = lower(std::string(endpoint.substr(0, 8)));
    return normalized.starts_with("http://") || normalized.starts_with("https://");
}

std::expected<std::string, RequestError> http_endpoint(std::string endpoint,
                                                       std::string_view option, bool use_https) {
    if (starts_with_scheme(endpoint))
        return endpoint;
    if (endpoint.find("://") != std::string::npos) {
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, std::string(option) + " must use HTTP or HTTPS"});
    }
    return std::string(use_https ? "https://" : "http://") + endpoint;
}

std::expected<UrlParts, RequestError> split_url(std::string_view url) {
    const std::size_t marker = url.find("://");
    if (marker == std::string_view::npos)
        return std::unexpected(RequestError{KARU_ERR_CONFIG, "endpoint has no URL scheme"});
    const std::size_t authority_start = marker + 3;
    const std::size_t path_start = url.find_first_of("/?", authority_start);
    UrlParts parts;
    parts.scheme = lower(std::string(url.substr(0, marker)));
    if (parts.scheme != "http" && parts.scheme != "https") {
        return std::unexpected(RequestError{KARU_ERR_CONFIG, "endpoint must use HTTP or HTTPS"});
    }
    parts.authority = std::string(url.substr(authority_start, path_start == std::string_view::npos
                                                                  ? url.size() - authority_start
                                                                  : path_start - authority_start));
    if (parts.authority.empty())
        return std::unexpected(RequestError{KARU_ERR_CONFIG, "endpoint has no authority"});
    if (parts.authority.find('@') != std::string::npos)
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "endpoint must not contain user information"});
    if (url.find('#', authority_start) != std::string_view::npos)
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "endpoint must not contain a fragment"});
    if (path_start == std::string_view::npos) {
        parts.path = "/";
        return parts;
    }
    const std::size_t query = url.find('?', path_start);
    parts.path = query == std::string_view::npos
                     ? std::string(url.substr(path_start))
                     : std::string(url.substr(path_start, query - path_start));
    if (parts.path.empty() || parts.path.front() == '?')
        parts.path = "/";
    if (query != std::string_view::npos)
        parts.query = std::string(url.substr(query + 1));
    return parts;
}

std::string append_object(const std::string& endpoint, std::string_view container,
                          std::string_view key) {
    return without_trailing_slash(endpoint) + "/" + std::string(container) + "/" + encode_path(key);
}

std::string form_encode(std::string_view text) {
    static constexpr char DIGITS[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(DIGITS[c >> 4]);
            result.push_back(DIGITS[c & 0x0f]);
        }
    }
    return result;
}

} // namespace karu::backends
