#include "transport.hpp"

#include "../text.hpp"
#include "http_response.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

namespace karu::transport {
namespace {

// Finishing a small unwanted body keeps the connection reusable. Larger ones
// are cut short rather than wasting bandwidth.
constexpr std::uint64_t kDrainLimit = 256u << 10;

Failure timeout_failure(std::string_view uri,
                        std::string_view reason = "request timeout exceeded") {
    return {KARU_TIMEOUT, concat(redact_url(uri), ": ", reason)};
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

bool uses_http2(HttpVersion version) noexcept {
    return version == HttpVersion::Http2Tls || version == HttpVersion::Http2PriorKnowledge;
}

std::optional<std::size_t> callback_size(std::size_t size, std::size_t count) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return std::nullopt;
    }
    return size * count;
}

// HTTP case comparisons must not depend on the application's locale.
constexpr unsigned char ascii_lower(unsigned char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool header_name_is(std::string_view line, std::string_view name) noexcept {
    if (line.size() < name.size())
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(line[index])) !=
            ascii_lower(static_cast<unsigned char>(name[index]))) {
            return false;
        }
    }
    return true;
}

bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
        return ascii_lower(a) == ascii_lower(b);
    });
}

bool same_origin_redirect(std::string_view original, std::string_view location) noexcept {
    // Resolve the Location with the same URL implementation that follows it.
    // Besides avoiding two subtly different parsers, this correctly handles
    // colons in relative paths, queries and fragments.
    try {
        const std::string original_copy(original);
        const std::string location_copy(location);
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
        if (!url || curl_url_set(url.get(), CURLUPART_URL, original_copy.c_str(), 0) != CURLUE_OK) {
            return false;
        }

        using CurlText = std::unique_ptr<char, decltype(&curl_free)>;
        const auto part = [&](CURLUPart requested, unsigned flags) -> CurlText {
            char* value = nullptr;
            if (curl_url_get(url.get(), requested, &value, flags) != CURLUE_OK)
                value = nullptr;
            return CurlText(value, curl_free);
        };
        CurlText source_scheme = part(CURLUPART_SCHEME, 0);
        CurlText source_host = part(CURLUPART_HOST, 0);
        CurlText source_port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
        if (!source_scheme || !source_host || !source_port ||
            curl_url_set(url.get(), CURLUPART_URL, location_copy.c_str(), 0) != CURLUE_OK) {
            return false;
        }

        CurlText target_scheme = part(CURLUPART_SCHEME, 0);
        CurlText target_host = part(CURLUPART_HOST, 0);
        CurlText target_port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
        return target_scheme && target_host && target_port &&
               ascii_equal(source_scheme.get(), target_scheme.get()) &&
               ascii_equal(source_host.get(), target_host.get()) &&
               std::string_view(source_port.get()) == target_port.get();
    } catch (...) {
        // Allocation failure while validating a redirect must fail closed.
        return false;
    }
}

long response_status(std::string_view line) noexcept {
    const std::size_t first = line.find(' ');
    if (first == std::string_view::npos)
        return 0;
    const std::size_t begin = line.find_first_not_of(' ', first);
    const std::size_t end = line.find(' ', begin);
    if (begin == std::string_view::npos)
        return 0;
    long status = 0;
    const std::string_view text = line.substr(begin, end - begin);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), status);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? status : 0;
}

std::string_view header_value(std::string_view line, std::string_view name) noexcept {
    std::string_view value = line.substr(name.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_content_range(std::string_view value, std::uint64_t& first, std::uint64_t& last,
                         std::uint64_t& total, bool& have_total) noexcept {
    constexpr std::string_view prefix = "bytes ";
    if (!value.starts_with(prefix))
        return false;
    value.remove_prefix(prefix.size());
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash == 0 ||
        dash > slash) {
        return false;
    }
    if (!parse_u64(value.substr(0, dash), first) ||
        !parse_u64(value.substr(dash + 1, slash - dash - 1), last) || last < first) {
        return false;
    }
    const std::string_view total_text = value.substr(slash + 1);
    have_total = total_text != "*";
    if (have_total && (!parse_u64(total_text, total) || total == 0 || last >= total))
        return false;
    return !total_text.empty();
}

bool parse_unsatisfied_content_range(std::string_view value, std::uint64_t& total) noexcept {
    constexpr std::string_view prefix = "bytes */";
    return value.starts_with(prefix) && parse_u64(value.substr(prefix.size()), total);
}

bool valid_content_range(const Transfer& transfer) noexcept {
    if (!transfer.content_range_seen || !transfer.content_range_valid ||
        transfer.content_range_start != transfer.offset + transfer.resumed) {
        return false;
    }
    const std::uint64_t requested_end = transfer.offset + transfer.length - 1;
    if (transfer.content_range_end > requested_end)
        return false;
    return transfer.content_range_end == requested_end ||
           (transfer.content_range_has_total &&
            transfer.content_range_end + 1 == transfer.content_range_total);
}

bool is_strong_etag(std::string_view value) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    value.remove_prefix(1);
    value.remove_suffix(1);
    return std::ranges::all_of(value,
                               [](unsigned char c) { return c >= 0x21 && c != '"' && c != 0x7f; });
}

// Catch servers that ignore a single-tag If-Match. The server evaluates lists and "*".
bool contradicts_pin(const Transfer& transfer) noexcept {
    const std::string_view pin = version_pin(transfer);
    return is_strong_etag(pin) && !transfer.response_etag.empty() && transfer.response_etag != pin;
}

template <std::size_t Capacity>
void capture_error_body(std::array<char, Capacity>& destination, std::size_t& used,
                        const char* data, std::size_t size) noexcept {
    const std::size_t take = std::min(destination.size() - used, size);
    if (take > 0) {
        std::memcpy(destination.data() + used, data, take);
        used += take;
    }
}

void record_encoding(EncodingHeaders& encoding, std::string_view line) {
    // Bound storage for untrusted encoding headers.
    constexpr std::size_t kLimit = 64;
    if (header_name_is(line, "content-encoding:")) {
        encoding.content = std::string(header_value(line, "content-encoding:").substr(0, kLimit));
    } else if (header_name_is(line, "x-goog-stored-content-encoding:")) {
        encoding.stored =
            std::string(header_value(line, "x-goog-stored-content-encoding:").substr(0, kLimit));
    } else if (header_name_is(line, "warning:")) {
        // GCS still uses Warning 214 for transcoding despite RFC 9111 retiring it.
        const std::string_view value = header_value(line, "warning:");
        if (value == "214" || value.starts_with("214 "))
            encoding.transformation_warning = true;
    }
}

void reset_response(Transfer& transfer) noexcept {
    transfer.received = transfer.resumed;
    transfer.response_etag.clear();
    transfer.version_changed = false;
    transfer.scatter_cursor = 0;
    transfer.skip = 0;
    transfer.body_length = -1;
    transfer.consume = 0;
    transfer.satisfied = false;
    transfer.checked_status = false;
    transfer.http_status = 0;
    transfer.redirect_blocked = false;
    transfer.content_range_seen = false;
    transfer.content_range_valid = false;
    transfer.content_range_matches = false;
    transfer.content_range_start = 0;
    transfer.content_range_end = 0;
    transfer.content_range_total = 0;
    transfer.content_range_has_total = false;
    transfer.range_fallback_rejected = false;
    transfer.encoding = {};
    transfer.response_region.clear();
    transfer.retry_after = 0;
    transfer.error_body_size = 0;
    transfer.error_body_received = 0;
}

std::size_t write_callback(char* data, std::size_t size, std::size_t count,
                           void* userdata) noexcept {
    const auto incoming = callback_size(size, count);
    if (!incoming)
        return 0;
    auto& transfer = *static_cast<Transfer*>(userdata);
    std::size_t remaining = *incoming;

    if (transfer.satisfied) {
        transfer.batch->counters.add(transfer.batch->counters.received_bytes, *incoming);
        return *incoming;
    }

    if (!transfer.checked_status) {
        transfer.checked_status = true;
        curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &transfer.http_status);
        if (transfer.http_status >= 200 && transfer.http_status < 300 &&
            detail::transformed(transfer.encoding)) {
            return 0;
        }
        if (transfer.http_status >= 200 && transfer.http_status < 300 &&
            contradicts_pin(transfer)) {
            // Reject the new version before copying any of its bytes.
            transfer.version_changed = true;
            return 0;
        }
        if (transfer.http_status == 206 && !valid_content_range(transfer)) {
            return 0;
        }
        if (transfer.http_status == 200 && transfer.resumed > 0) {
            // Range was ignored. Restart from this full body so the fallback
            // limit applies to the original offset, not offset + resumed.
            transfer.resumed = 0;
            transfer.received = 0;
        }
        const std::uint64_t first = transfer.offset + transfer.resumed;
        if (transfer.http_status == 200 && first > 0) {
            if (first > transfer.range_fallback_limit) {
                transfer.range_fallback_rejected = true;
                return 0;
            }
            transfer.skip = first;
        }
        curl_off_t body_length = -1;
        curl_easy_getinfo(transfer.easy.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &body_length);
        transfer.body_length = body_length;
        const std::uint64_t wanted = transfer.length - transfer.resumed;
        transfer.consume = transfer.skip > std::numeric_limits<std::uint64_t>::max() - wanted
                               ? std::numeric_limits<std::uint64_t>::max()
                               : transfer.skip + wanted;
    }

    if (transfer.http_status >= 200 && transfer.http_status < 300)
        transfer.batch->counters.add(transfer.batch->counters.received_bytes, *incoming);

    if (transfer.http_status >= 300 && transfer.http_status < 400 &&
        transfer.http->follow_redirects) {
        return *incoming;
    }

    if (transfer.http_status >= 300) {
        capture_error_body(transfer.http_buffers->error_body, transfer.error_body_size, data,
                           *incoming);
        if (*incoming > kDrainLimit - std::min(kDrainLimit, transfer.error_body_received)) {
            return 0;
        }
        transfer.error_body_received += *incoming;
        return *incoming;
    }

    if (transfer.skip > 0) {
        const auto drop =
            static_cast<std::size_t>(std::min<std::uint64_t>(transfer.skip, remaining));
        data += drop;
        remaining -= drop;
        transfer.skip -= drop;
    }
    if (remaining == 0)
        return *incoming;

    const std::uint64_t room = transfer.length - transfer.received;
    const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(room, remaining));
    if (take > 0)
        store_payload(transfer, data, take);

    if (transfer.received == transfer.length) {
        transfer.satisfied = true;
        std::uint64_t excess = std::numeric_limits<std::uint64_t>::max();
        if (transfer.body_length >= 0) {
            const auto body = static_cast<std::uint64_t>(transfer.body_length);
            excess = body > transfer.consume ? body - transfer.consume : 0;
        } else if (take == remaining && transfer.http_status != 200) {
            excess = 0;
        }
        if (excess > kDrainLimit)
            return 0;
    }
    return *incoming;
}

std::size_t header_callback(char* data, std::size_t size, std::size_t count,
                            void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& transfer = *static_cast<Transfer*>(userdata);
    const std::string_view line(data, *bytes);

    // libcurl reports a new status line for redirects and interim responses.
    if (line.starts_with("HTTP/")) {
        reset_response(transfer);
        transfer.http_status = response_status(line);
    } else if (header_name_is(line, "location:") && transfer.http_status >= 300 &&
               transfer.http_status < 400 && transfer.http->follow_redirects &&
               transfer.http->same_origin_redirects_only &&
               !same_origin_redirect(transfer.request_url, header_value(line, "location:"))) {
        transfer.redirect_blocked = true;
        return 0;
    } else if (header_name_is(line, "content-range:")) {
        transfer.content_range_seen = true;
        transfer.content_range_valid =
            parse_content_range(header_value(line, "content-range:"), transfer.content_range_start,
                                transfer.content_range_end, transfer.content_range_total,
                                transfer.content_range_has_total);
        if (transfer.content_range_valid)
            transfer.content_range_matches = valid_content_range(transfer);
    } else if (header_name_is(line, "x-amz-bucket-region:")) {
        transfer.response_region = std::string(header_value(line, "x-amz-bucket-region:"));
    } else if (header_name_is(line, "retry-after:")) {
        transfer.retry_after =
            detail::retry_after_seconds(header_value(line, "retry-after:"), std::time(nullptr));
    } else if (header_name_is(line, "etag:")) {
        // Only a strong validator can pin a resumed range with If-Match.
        const std::string_view value = header_value(line, "etag:");
        if (value.size() <= 255 && is_strong_etag(value))
            transfer.response_etag = std::string(value);
    } else {
        record_encoding(transfer.encoding, line);
    }
    return *bytes;
}

std::expected<Slist, std::string>
build_headers(const std::vector<std::pair<std::string, std::string>>& values,
              std::string_view range) {
    Slist headers;
    auto append = [&](std::string_view name,
                      std::string_view value) -> std::expected<void, std::string> {
        if (name.empty() || name.find_first_of(":\r\n") != std::string_view::npos ||
            value.find_first_of("\r\n") != std::string_view::npos) {
            return std::unexpected("invalid HTTP header name or value");
        }
        std::string line;
        line.reserve(name.size() + value.size() + 2);
        line.append(name);
        line.append(": ");
        line.append(value);
        curl_slist* appended = curl_slist_append(headers.get(), line.c_str());
        if (appended == nullptr)
            return std::unexpected("out of memory building HTTP headers");
        static_cast<void>(headers.release());
        headers.reset(appended);
        return {};
    };
    for (const auto& [name, value] : values) {
        if (auto added = append(name, value); !added)
            return std::unexpected(added.error());
    }
    if (auto added = append("Range", range); !added)
        return std::unexpected(added.error());
    return headers;
}

template <typename Value>
std::expected<void, std::string> set_option(CURL* easy, CURLoption option, Value value) {
    const CURLcode code = curl_easy_setopt(easy, option, value);
    if (code == CURLE_OK)
        return {};
    if (const curl_easyoption* details = curl_easy_option_by_id(option); details != nullptr) {
        return std::unexpected(concat("CURLOPT_", details->name, ": ", curl_easy_strerror(code)));
    }
    return std::unexpected(
        concat("curl option ", static_cast<long>(option), ": ", curl_easy_strerror(code)));
}

template <typename Value, typename... Rest>
std::expected<void, std::string> set_options(CURL* easy, CURLoption option, Value value,
                                             Rest... rest) {
    if (auto result = set_option(easy, option, value); !result)
        return result;
    if constexpr (sizeof...(rest) == 0) {
        return {};
    } else {
        return set_options(easy, rest...);
    }
}

std::expected<void, std::string> restrict_to_http(CURL* easy) {
#if LIBCURL_VERSION_NUM >= 0x075500
    return set_options(easy, CURLOPT_PROTOCOLS_STR, "http,https", CURLOPT_REDIR_PROTOCOLS_STR,
                       "http,https");
#else
    constexpr long protocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    return set_options(easy, CURLOPT_PROTOCOLS, protocols, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
}

// libcurl shuffles on resolution. Bypass its DNS cache so new connections
// get a fresh order; existing connections need no lookup.
std::expected<void, std::string> spread_connections(CURL* easy) {
    return set_options(easy, CURLOPT_DNS_SHUFFLE_ADDRESSES, 1L, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
}

// libcurl 8.7.0 shares one receive buffer per multi handle. Older
// versions allocate one per transfer, so use a smaller buffer there.
long receive_buffer_size() noexcept {
    static const long size = [] {
        const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        return info != nullptr && info->version_num >= 0x080700 ? 256L << 10 : 64L << 10;
    }();
    return size;
}

std::expected<void, std::string> apply_http_options(CURL* easy, const HttpRequestOptions& options) {
    if (auto result = set_options(easy, CURLOPT_FOLLOWLOCATION, options.follow_redirects ? 1L : 0L,
                                  CURLOPT_HTTP_VERSION, curl_http_version(options.version),
                                  CURLOPT_PIPEWAIT, uses_http2(options.version) ? 1L : 0L);
        !result) {
        return result;
    }
    const auto apply_string = [&](CURLoption option, const std::string& value) {
        return value.empty() ? std::expected<void, std::string>{}
                             : set_option(easy, option, value.c_str());
    };
    if (auto result = apply_string(CURLOPT_CAINFO, options.ca_bundle); !result)
        return result;
    if (auto result = apply_string(CURLOPT_CAPATH, options.ca_path); !result)
        return result;
    if (auto result = apply_string(CURLOPT_PROXY, options.proxy); !result)
        return result;
    if (auto result = apply_string(CURLOPT_PROXYUSERPWD, options.proxy_user_password); !result)
        return result;
    return apply_string(CURLOPT_USERAGENT, options.user_agent);
}

struct ProbeState {
    CURL* easy = nullptr;
    ByteRange range{};
    std::span<std::byte> destination{};
    std::string_view if_match;
    std::uint64_t fallback_limit = 0;
    bool follow_redirects = true;
    bool same_origin_redirects_only = false;
    std::string_view original_url;

    long http_status = 0;
    std::uint64_t total = 0;
    bool have_total = false;
    bool content_range_valid = false;
    bool content_range_answers = false;
    bool unsatisfied_content_range_valid = false;
    std::uint64_t range_first = 0;
    std::uint64_t range_last = 0;
    bool started = false;
    std::uint64_t skip = 0;
    std::uint64_t wanted = 0;
    std::uint64_t stored = 0;
    bool cut_short = false;
    bool body_exceeded_range = false;
    bool suffix_ignored = false;
    bool range_fallback_rejected = false;
    bool version_changed = false;
    bool redirect_blocked = false;
    int retry_after = 0;
    std::string response_region{};
    std::string etag{};
    EncodingHeaders encoding{};
    std::array<char, kErrorBodyCapacity> error_body{};
    std::size_t error_body_size = 0;
    std::uint64_t error_body_received = 0;
};

void reset_response(ProbeState& state) noexcept {
    state.http_status = 0;
    state.total = 0;
    state.have_total = false;
    state.content_range_valid = false;
    state.content_range_answers = false;
    state.unsatisfied_content_range_valid = false;
    state.range_first = 0;
    state.range_last = 0;
    state.started = false;
    state.skip = 0;
    state.wanted = 0;
    state.stored = 0;
    state.cut_short = false;
    state.body_exceeded_range = false;
    state.suffix_ignored = false;
    state.range_fallback_rejected = false;
    state.version_changed = false;
    state.redirect_blocked = false;
    state.retry_after = 0;
    state.response_region.clear();
    state.etag.clear();
    state.encoding = {};
    state.error_body_size = 0;
    state.error_body_received = 0;
}

// An absolute range may stop at the end of the object, and a suffix covers
// the whole object when the object is shorter.
bool answers(const ByteRange& range, std::uint64_t first, std::uint64_t last,
             std::uint64_t total) noexcept {
    if (range.suffix)
        return last + 1 == total && last - first + 1 == std::min(range.length, total);
    const std::uint64_t end = range.first + range.length - 1;
    return first == range.first && last <= end && (last == end || last + 1 == total);
}

// Decide where a successful body goes. False stops the transfer.
bool start_body(ProbeState& state, long status) noexcept {
    if (detail::transformed(state.encoding))
        return false;
    if (is_strong_etag(state.if_match) && !state.etag.empty() && state.etag != state.if_match) {
        state.version_changed = true;
        return false;
    }
    if (status == 206) {
        if (!state.content_range_answers)
            return false;
        state.wanted = state.range_last - state.range_first + 1;
        return true;
    }
    if (status != 200)
        return false;
    // The server ignored Range and sends the whole object.
    if (!state.have_total) {
        curl_off_t length = -1;
        curl_easy_getinfo(state.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        if (length < 0)
            return false;
        state.total = static_cast<std::uint64_t>(length);
        state.have_total = true;
    }
    if (state.range.suffix) {
        // A request by offset costs less than reading a longer object to its end.
        if (state.total > state.range.length) {
            state.suffix_ignored = true;
            return false;
        }
        state.range_first = 0;
        state.wanted = state.total;
        return true;
    }
    if (state.range.first > state.fallback_limit) {
        state.range_fallback_rejected = true;
        return false;
    }
    state.skip = state.range.first;
    state.range_first = state.range.first;
    state.wanted = state.range.first < state.total
                       ? std::min(state.range.length, state.total - state.range.first)
                       : 0;
    return true;
}

std::size_t probe_body_callback(char* data, std::size_t size, std::size_t count,
                                void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& state = *static_cast<ProbeState*>(userdata);
    long status = 0;
    curl_easy_getinfo(state.easy, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300 && status < 400 && state.follow_redirects)
        return *bytes;
    if (status >= 300) {
        capture_error_body(state.error_body, state.error_body_size, data, *bytes);
        if (*bytes > kDrainLimit - std::min(kDrainLimit, state.error_body_received))
            return 0;
        state.error_body_received += *bytes;
        return *bytes;
    }
    if (!state.started) {
        state.started = true;
        if (!start_body(state, status)) {
            state.cut_short = true;
            return 0;
        }
    }
    std::size_t remaining = *bytes;
    if (state.skip > 0) {
        const auto drop = static_cast<std::size_t>(std::min<std::uint64_t>(state.skip, remaining));
        data += drop;
        remaining -= drop;
        state.skip -= drop;
    }
    const std::uint64_t room = state.wanted - state.stored;
    if (status == 206 && remaining > room) {
        state.body_exceeded_range = true;
        return 0;
    }
    const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(room, remaining));
    if (take > 0) {
        std::memcpy(state.destination.data() + state.stored, data, take);
        state.stored += take;
    }
    // The rest of a whole-object body is not needed.
    if (status == 200 && state.skip == 0 && state.stored == state.wanted) {
        state.cut_short = true;
        return 0;
    }
    return *bytes;
}

std::size_t probe_header_callback(char* data, std::size_t size, std::size_t count,
                                  void* userdata) noexcept {
    const auto bytes = callback_size(size, count);
    if (!bytes)
        return 0;
    auto& state = *static_cast<ProbeState*>(userdata);
    const std::string_view line(data, *bytes);

    if (line.starts_with("HTTP/")) {
        reset_response(state);
        state.http_status = response_status(line);
    } else if (header_name_is(line, "location:") && state.http_status >= 300 &&
               state.http_status < 400 && state.follow_redirects &&
               state.same_origin_redirects_only &&
               !same_origin_redirect(state.original_url, header_value(line, "location:"))) {
        state.redirect_blocked = true;
        return 0;
    } else if (header_name_is(line, "content-range:")) {
        const std::string_view value = header_value(line, "content-range:");
        std::uint64_t parsed_total = 0;
        bool parsed_have_total = false;
        state.content_range_valid = parse_content_range(value, state.range_first, state.range_last,
                                                        parsed_total, parsed_have_total) &&
                                    parsed_have_total;
        if (state.content_range_valid) {
            state.total = parsed_total;
            state.have_total = true;
            state.content_range_answers =
                answers(state.range, state.range_first, state.range_last, parsed_total);
            return *bytes;
        }
        std::uint64_t unsatisfied_total = 0;
        state.unsatisfied_content_range_valid =
            parse_unsatisfied_content_range(value, unsatisfied_total);
        if (state.unsatisfied_content_range_valid) {
            state.total = unsatisfied_total;
            state.have_total = true;
        }
    } else if (header_name_is(line, "x-amz-bucket-region:")) {
        state.response_region = std::string(header_value(line, "x-amz-bucket-region:"));
    } else if (header_name_is(line, "retry-after:")) {
        state.retry_after =
            detail::retry_after_seconds(header_value(line, "retry-after:"), std::time(nullptr));
    } else if (header_name_is(line, "etag:")) {
        // A weak validator cannot be used with If-Match, so it is not reported.
        const std::string_view value = header_value(line, "etag:");
        if (value.size() <= 255 && is_strong_etag(value))
            state.etag = std::string(value);
    } else {
        record_encoding(state.encoding, line);
    }
    return *bytes;
}

} // namespace

std::string_view version_pin(const Transfer& transfer) noexcept {
    return transfer.if_match.empty() ? std::string_view(transfer.resume_etag)
                                     : std::string_view(transfer.if_match);
}

void plan_resume(Transfer& transfer) {
    if (!transfer.if_match.empty() && !is_strong_etag(transfer.if_match)) {
        // Lists and "*" may accept a different version. Restart the full range.
        transfer.resumed = 0;
        transfer.resume_etag.clear();
        return;
    }
    if (transfer.version_changed || (transfer.http_status == 412 && transfer.if_match.empty())) {
        // Discard the old prefix before retrying after a version change.
        transfer.resumed = 0;
        transfer.resume_etag.clear();
        return;
    }
    const bool data = transfer.http_status == 200 || transfer.http_status == 206;
    if (!data || transfer.received <= transfer.resumed || transfer.received >= transfer.length)
        return;
    if (transfer.if_match.empty() && transfer.resume_etag.empty()) {
        if (transfer.response_etag.empty()) {
            // Nothing identifies this version, so the retry starts over.
            transfer.resumed = 0;
            return;
        }
        transfer.resume_etag = transfer.response_etag;
    }
    transfer.resumed = transfer.received;
}

bool ensure_sink(Transfer& transfer) noexcept {
    if (transfer.sink != nullptr)
        return true;
    if (!transfer.scattered) {
        transfer.sink = static_cast<std::byte*>(transfer.parts.front().buffer);
        return true;
    }
    // Curl can scatter each incoming chunk directly. pread needs one contiguous
    // destination, so coalesced local reads use scratch space.
    if (transfer.locator->resolved.backend != Backend::File)
        return true;
    if (transfer.length > std::numeric_limits<std::size_t>::max())
        return false;
    try {
        transfer.scratch.resize(static_cast<std::size_t>(transfer.length));
    } catch (...) {
        return false;
    }
    transfer.sink = transfer.scratch.data();
    return true;
}

void store_payload(Transfer& transfer, const void* data, std::size_t size) noexcept {
    const auto* source = static_cast<const std::byte*>(data);
    const std::uint64_t chunk_begin = transfer.received;
    const std::uint64_t chunk_end = chunk_begin + size;

    if (!transfer.scattered) {
        std::memcpy(transfer.sink + static_cast<std::size_t>(chunk_begin), source, size);
        transfer.received = chunk_end;
        return;
    }

    // Parts may overlap. Advance only past parts that this chunk cannot touch.
    while (transfer.scatter_cursor < transfer.parts.size()) {
        const Part& part = transfer.parts[transfer.scatter_cursor];
        if (part.relative_offset + part.length > chunk_begin)
            break;
        ++transfer.scatter_cursor;
    }

    for (std::size_t index = transfer.scatter_cursor; index < transfer.parts.size(); ++index) {
        const Part& part = transfer.parts[index];
        const std::uint64_t part_begin = part.relative_offset;
        if (part_begin >= chunk_end)
            break;
        const std::uint64_t part_end = part_begin + part.length;
        const std::uint64_t begin = std::max(chunk_begin, part_begin);
        const std::uint64_t end = std::min(chunk_end, part_end);
        if (begin >= end)
            continue;
        std::memcpy(static_cast<std::byte*>(part.buffer) +
                        static_cast<std::size_t>(begin - part_begin),
                    source + static_cast<std::size_t>(begin - chunk_begin),
                    static_cast<std::size_t>(end - begin));
    }
    transfer.received = chunk_end;
}

std::expected<void, std::string> configure(Transfer& transfer, CURLSH* share,
                                           const ClientOptions& options) {
    if (!transfer.http || !transfer.http_buffers)
        return std::unexpected("HTTP transfer state is not initialized");
    reset_response(transfer);
    transfer.range_fallback_limit = options.range_fallback_limit;
    transfer.http_buffers->error[0] = '\0';

    auto headers = build_headers(transfer.request_headers, transfer.request_range);
    if (!headers)
        return std::unexpected(headers.error());
    transfer.headers = std::move(*headers);

    auto configured = set_options(
        transfer.easy.get(), CURLOPT_URL, transfer.url().c_str(), CURLOPT_WRITEFUNCTION,
        write_callback, CURLOPT_WRITEDATA, &transfer, CURLOPT_HEADERFUNCTION, header_callback,
        CURLOPT_HEADERDATA, &transfer, CURLOPT_PRIVATE, &transfer, CURLOPT_SHARE, share,
        CURLOPT_ERRORBUFFER, transfer.http_buffers->error.data(), CURLOPT_NOSIGNAL, 1L,
        CURLOPT_PATH_AS_IS, 1L, CURLOPT_UNRESTRICTED_AUTH, 0L, CURLOPT_MAXREDIRS, 10L,
        CURLOPT_TCP_KEEPALIVE, 1L, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds,
        CURLOPT_TIMEOUT_MS, transfer.deadline.curl_timeout_ms(), CURLOPT_LOW_SPEED_LIMIT,
        options.low_speed_limit, CURLOPT_LOW_SPEED_TIME, options.low_speed_time_seconds,
        CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (!configured)
        return configured;
    configured = spread_connections(transfer.easy.get());
    if (!configured)
        return configured;
    // Older libcurl may reject resizing a pooled handle's buffer. Its
    // existing buffer remains usable, so this failure is harmless.
    static_cast<void>(
        curl_easy_setopt(transfer.easy.get(), CURLOPT_BUFFERSIZE, receive_buffer_size()));
    if (auto protocols = restrict_to_http(transfer.easy.get()); !protocols)
        return protocols;
    if (auto http = apply_http_options(transfer.easy.get(), *transfer.http); !http)
        return http;
    if (transfer.headers) {
        return set_option(transfer.easy.get(), CURLOPT_HTTPHEADER, transfer.headers.get());
    }
    return {};
}

std::expected<RangeReply, Failure> get_range(const Locator& locator, CURL* easy, CURLSH* share,
                                             RequestBuilder& request_builder,
                                             const ClientOptions& options, const ByteRange& range,
                                             std::span<std::byte> destination,
                                             std::string_view if_match) {
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    ProbeState state;
    auto configured =
        set_options(easy, CURLOPT_HEADERFUNCTION, probe_header_callback, CURLOPT_HEADERDATA, &state,
                    CURLOPT_WRITEFUNCTION, probe_body_callback, CURLOPT_WRITEDATA, &state,
                    CURLOPT_SHARE, share, CURLOPT_ERRORBUFFER, error_buffer.data(),
                    CURLOPT_NOSIGNAL, 1L, CURLOPT_PATH_AS_IS, 1L, CURLOPT_UNRESTRICTED_AUTH, 0L,
                    CURLOPT_MAXREDIRS, 10L, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds,
                    CURLOPT_LOW_SPEED_LIMIT, options.low_speed_limit, CURLOPT_LOW_SPEED_TIME,
                    options.low_speed_time_seconds, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (configured)
        configured = spread_connections(easy);
    if (!configured) {
        return std::unexpected(Failure{KARU_ERR_NETWORK, configured.error()});
    }
    if (auto protocols = restrict_to_http(easy); !protocols) {
        return std::unexpected(Failure{KARU_ERR_NETWORK, protocols.error()});
    }

    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 random{static_cast<std::mt19937::result_type>(seed)};
    std::string region_hint;
    bool region_retried = false;
    bool credentials_retried = false;
    RequestDeadline deadline;
    deadline.start(options.request_timeout_seconds);
    int unreachable_attempts = 0;
    for (int attempt = 0; attempt < options.max_attempts;) {
        if (deadline.expired())
            return std::unexpected(timeout_failure(locator.resolved.canonical_uri));
        auto request = range.suffix ? request_builder.prepare_suffix(locator, range.length,
                                                                     region_hint, if_match)
                                    : request_builder.prepare(locator, range.first, range.length,
                                                              region_hint, if_match);
        if (!request) {
            return std::unexpected(Failure{request.error().status, request.error().message});
        }
        if (deadline.expired())
            return std::unexpected(timeout_failure(request->url));
        auto headers = build_headers(request->headers, request->range);
        if (!headers) {
            return std::unexpected(Failure{KARU_ERR_NOMEM, headers.error()});
        }
        if (auto result = set_option(easy, CURLOPT_HTTPHEADER, headers->get()); !result) {
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        }
        if (auto result = set_option(easy, CURLOPT_URL, request->url.c_str()); !result) {
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        }
        if (auto result = apply_http_options(easy, request->http); !result)
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        if (auto result = set_option(easy, CURLOPT_TIMEOUT_MS, deadline.curl_timeout_ms());
            !result) {
            return std::unexpected(Failure{KARU_ERR_NETWORK, result.error()});
        }
        region_hint = request->routing_region;

        state = ProbeState{.easy = easy,
                           .range = range,
                           .destination = destination,
                           .if_match = if_match,
                           .fallback_limit = options.range_fallback_limit,
                           .follow_redirects = request->http.follow_redirects,
                           .same_origin_redirects_only = request->http.same_origin_redirects_only,
                           .original_url = request->url};
        error_buffer[0] = '\0';
        CURLcode code = curl_easy_perform(easy);
        long http_status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status);
        if (code == CURLE_WRITE_ERROR && state.cut_short)
            code = CURLE_OK;
        // An empty body never reached the callback.
        if (code == CURLE_OK && http_status >= 200 && http_status < 300 && !state.started) {
            state.started = true;
            static_cast<void>(start_body(state, http_status));
        }

        if (state.redirect_blocked) {
            return std::unexpected(Failure{
                KARU_ERR_HTTP,
                concat(redact_url(request->url),
                       ": cross-origin redirect blocked because KARU_HTTP_HEADERS is set")});
        }

        const std::string_view error_body(state.error_body.data(), state.error_body_size);
        if (state.response_region.empty() && locator.resolved.backend == Backend::S3)
            state.response_region = detail::s3_region(error_body);

        if (http_status >= 300 && !region_retried && !state.response_region.empty() &&
            state.response_region != region_hint && locator.resolved.backend == Backend::S3) {
            region_hint = state.response_region;
            region_retried = true;
            continue;
        }

        if (!credentials_retried &&
            detail::credentials_expired(locator.resolved.backend, http_status, error_body)) {
            request_builder.invalidate_credentials(locator);
            credentials_retried = true;
            continue;
        }

        if (code == CURLE_OK && http_status == 416) {
            if (!state.unsatisfied_content_range_valid) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP, concat(redact_url(request->url),
                                                  ": invalid Content-Range in HTTP 416")});
            }
            // No byte of the object is in the range.
            if (range.suffix ? state.total == 0 : range.first >= state.total) {
                return RangeReply{.total = state.total,
                                  .first = range.suffix ? 0 : range.first,
                                  .got = 0,
                                  .etag = state.etag};
            }
            if (range.suffix) {
                return RangeReply{.total = state.total, .etag = state.etag, .suffix_ignored = true};
            }
        }

        if (http_status == 206 && state.body_exceeded_range) {
            return std::unexpected(Failure{
                KARU_ERR_HTTP, concat(redact_url(request->url), ": response exceeded bytes ",
                                      state.range_first, "-", state.range_last)});
        }

        if (code == CURLE_OK && http_status >= 200 && http_status < 300 &&
            detail::transformed(state.encoding)) {
            return std::unexpected(
                Failure{KARU_ERR_HTTP,
                        detail::transformation_detail(redact_url(request->url), state.encoding)});
        }

        if (code == CURLE_OK && state.version_changed) {
            return std::unexpected(Failure{
                KARU_ERR_PRECONDITION,
                concat(redact_url(request->url), ": the server ignored If-Match and sent ETag ",
                       state.etag, ", so the object no longer matches if_match")});
        }

        if (code == CURLE_OK && http_status == 206) {
            if (!state.content_range_answers) {
                return std::unexpected(Failure{
                    KARU_ERR_HTTP,
                    state.content_range_valid
                        ? concat(redact_url(request->url), ": Content-Range [", state.range_first,
                                 ", ", state.range_last, "] does not answer ", request->range)
                        : concat(redact_url(request->url),
                                 ": partial response has no valid object size")});
            }
            if (state.stored != state.wanted) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP, concat(redact_url(request->url),
                                                  ": response body does not match Content-Range")});
            }
            return RangeReply{state.total, state.range_first, state.stored, state.etag};
        }

        if (code == CURLE_OK && http_status == 200) {
            if (state.suffix_ignored)
                return RangeReply{.total = state.total, .etag = state.etag, .suffix_ignored = true};
            if (state.range_fallback_rejected) {
                return std::unexpected(Failure{
                    KARU_ERR_HTTP,
                    concat(redact_url(request->url), ": server ignored Range; refusing to discard ",
                           range.first, " bytes (limit ", options.range_fallback_limit, ")")});
            }
            if (!state.have_total) {
                return std::unexpected(
                    Failure{KARU_ERR_HTTP,
                            concat(redact_url(request->url), ": the server reported no size")});
            }
            return RangeReply{state.total, state.range_first, state.stored, state.etag};
        }

        if (code == CURLE_OK && http_status >= 200 && http_status < 300) {
            return std::unexpected(
                Failure{KARU_ERR_HTTP, concat(redact_url(request->url), ": unexpected HTTP ",
                                              http_status, " response to a range request")});
        }

        if (deadline.expired())
            return std::unexpected(timeout_failure(request->url));

        ++attempt;
        if (unreachable(code))
            ++unreachable_attempts;
        const bool request_timeout =
            http_status == 400 && error_body.find("RequestTimeout") != std::string_view::npos;
        if (attempt < options.max_attempts && unreachable_attempts < kUnreachableAttempts &&
            (detail::transient(code, http_status) || request_timeout)) {
            const auto wait = detail::retry_delay(attempt, state.retry_after, random);
            if (!deadline.can_wait_for(wait))
                return std::unexpected(timeout_failure(
                    request->url, "request timeout leaves no time for another attempt"));
            std::this_thread::sleep_for(wait);
            continue;
        }

        if (code != CURLE_OK && http_status >= 300 && http_status < 400 &&
            request->http.follow_redirects) {
            const char* message =
                error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code);
            return std::unexpected(
                Failure{KARU_ERR_NETWORK, concat(redact_url(request->url), ": ", message)});
        }
        if (http_status >= 300) {
            std::string detail = concat(redact_url(request->url), ": HTTP ", http_status);
            if (const std::string body = detail::summarize(error_body); !body.empty()) {
                detail += ": " + body;
            }
            return std::unexpected(
                Failure{detail::status_for_http(http_status), std::move(detail)});
        }
        const char* message =
            error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code);
        return std::unexpected(
            Failure{KARU_ERR_NETWORK, concat(redact_url(request->url), ": ", message)});
    }

    return std::unexpected(Failure{KARU_ERR_NETWORK, "range request exhausted its retries"});
}

std::expected<ObjectInfo, Failure> object_info(const Locator& locator, CURL* easy, CURLSH* share,
                                               RequestBuilder& request_builder,
                                               const ClientOptions& options) {
    // A one-byte GET is more dependable than HEAD across object stores; the
    // total comes from Content-Range and no object metadata is retained.
    std::array<std::byte, 1> first{};
    auto reply =
        get_range(locator, easy, share, request_builder, options, ByteRange{0, 1, false}, first);
    if (!reply)
        return std::unexpected(std::move(reply.error()));
    return ObjectInfo{reply->total, std::move(reply->etag)};
}

} // namespace karu::transport
