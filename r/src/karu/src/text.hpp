#ifndef KARU_TEXT_HPP
#define KARU_TEXT_HPP

#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>

namespace karu {

inline void append_text(std::string& output, std::string_view value) {
    output.append(value);
}

template <std::integral Integer> void append_text(std::string& output, Integer value) {
    output.append(std::to_string(value));
}

template <class... Parts> [[nodiscard]] std::string concat(Parts&&... parts) {
    std::string output;
    (append_text(output, std::forward<Parts>(parts)), ...);
    return output;
}

[[nodiscard]] inline std::string redact_url(std::string_view url) {
    std::string result(url);
    const std::size_t scheme = result.find("://");
    if (scheme != std::string::npos) {
        const std::size_t authority = scheme + 3;
        const std::size_t authority_end = result.find_first_of("/?#", authority);
        const std::size_t at = result.find('@', authority);
        if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
            result.replace(authority, at - authority, "<redacted>");
        }
    }
    const std::size_t query = result.find('?');
    const std::size_t fragment = result.find('#');
    const std::size_t secret = std::min(query, fragment);
    if (secret != std::string::npos) {
        const bool had_query = query == secret;
        result.resize(secret);
        result += had_query ? "?<redacted>" : "#<redacted>";
    }
    return result;
}

} // namespace karu

#endif
