#ifndef KARU_TRANSFER_HPP
#define KARU_TRANSFER_HPP

#include "../buffer.hpp"
#include "../locator.hpp"
#include "../request.hpp"
#include "batch.hpp"
#include "curl_types.hpp"
#include "deadline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <memory>
#include <string>
#include <vector>

namespace karu {

inline constexpr std::size_t kErrorBodyCapacity = 4u << 10;

struct Request {
    const Locator* locator = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    void* buffer = nullptr;
    void* tag = nullptr;
    std::string if_match;
};

struct Part {
    std::uint64_t relative_offset = 0;
    std::uint64_t length = 0;
    void* buffer = nullptr;
    void* tag = nullptr;
    OwnedBuffer owned_buffer;
};

// A transformed body may no longer use the stored object's byte offsets.
struct EncodingHeaders {
    std::string content;
    std::string stored;
    bool transformation_warning = false;
};

struct HttpBuffers {
    std::array<char, kErrorBodyCapacity> error_body{};
    std::array<char, CURL_ERROR_SIZE> error{};
};

struct Transfer {
    BatchCore* batch = nullptr;
    std::shared_ptr<const Locator> locator;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::vector<Part> parts;
    std::string if_match;

    bool scattered = false;
    std::byte* sink = nullptr;
    std::vector<std::byte> scratch;
    std::size_t scatter_cursor = 0;

    // Rebuilt for every attempt after the byte range is known. This keeps
    // renewable credentials and signatures out of the locator.
    std::string request_url;
    std::string request_range;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::unique_ptr<HttpRequestOptions> http;
    std::string region_hint;
    ResolvedCredentials credentials;
    bool credentials_ready = false;

    // Keep partial data only when a strong ETag pins it to one version.
    // resumed counts bytes retained; resume_etag supplies the pin when the
    // caller gave none. version_changed marks a mismatched response ETag.
    std::uint64_t resumed = 0;
    std::string resume_etag;
    std::string response_etag;
    bool version_changed = false;

    std::uint64_t received = 0;
    std::uint64_t skip = 0;
    std::int64_t body_length = -1;
    std::uint64_t consume = 0;
    bool satisfied = false;
    bool checked_status = false;
    long http_status = 0;
    bool redirect_blocked = false;
    bool content_range_seen = false;
    bool content_range_valid = false;
    bool content_range_matches = false;
    std::uint64_t content_range_start = 0;
    std::uint64_t content_range_end = 0;
    std::uint64_t content_range_total = 0;
    bool content_range_has_total = false;
    std::uint64_t range_fallback_limit = 0;
    bool range_fallback_rejected = false;
    EncodingHeaders encoding{};
    std::string response_region;
    bool region_retried = false;
    bool credentials_retried = false;
    int attempt = 0;
    // Attempts that failed to resolve the host or to connect to it.
    int unreachable_attempts = 0;
    RequestDeadline deadline;

    int retry_after = 0;
    std::size_t error_body_size = 0;
    std::uint64_t error_body_received = 0;
    std::unique_ptr<HttpBuffers> http_buffers;

    Easy easy;
    Slist headers;

    [[nodiscard]] const std::string& url() const noexcept { return request_url; }
};

} // namespace karu

#endif
