#ifndef KARU_TRANSPORT_HPP
#define KARU_TRANSPORT_HPP

#include "../config.hpp"
#include "../locator.hpp"
#include "../request_builder.hpp"
#include "curl_types.hpp"
#include "transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace karu::transport {

struct Failure {
    karu_status status;
    std::string detail;
};

[[nodiscard]] bool ensure_sink(Transfer& transfer) noexcept;

// Store one response-body chunk at the current transfer position. Merged HTTP
// transfers write only the intersections requested by their parts; gaps are
// consumed without being staged.
void store_payload(Transfer& transfer, const void* data, std::size_t size) noexcept;

[[nodiscard]] std::expected<void, std::string> configure(Transfer& transfer, CURLSH* share,
                                                         const ClientOptions& options);

// Keep a partial response only if the retry can request the same version.
void plan_resume(Transfer& transfer);
// Prefer the caller's condition over an ETag saved for resumption.
[[nodiscard]] std::string_view version_pin(const Transfer& transfer) noexcept;

[[nodiscard]] bool succeeded(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] bool retryable(const Transfer& transfer, CURLcode code) noexcept;
[[nodiscard]] Failure failure(const Transfer& transfer, CURLcode code);

// Limit DNS and connection failures separately from server-side retries.
// Connection and request timeouts still bound how long each attempt can take.
inline constexpr int kUnreachableAttempts = 3;
[[nodiscard]] bool unreachable(CURLcode code) noexcept;

struct ObjectInfo {
    std::uint64_t size = 0;
    // Strong entity tag with its quotes, or empty.
    std::string etag;
};

[[nodiscard]] std::expected<ObjectInfo, Failure> object_info(const Locator& locator, CURL* easy,
                                                             CURLSH* share,
                                                             RequestBuilder& request_builder,
                                                             const ClientOptions& options);

// Absolute offsets. A suffix asks for the last `length` bytes of the object.
struct ByteRange {
    std::uint64_t first = 0;
    std::uint64_t length = 0;
    bool suffix = false;
};

struct RangeReply {
    std::uint64_t total = 0;
    // The bytes stored at the destination start at this offset.
    std::uint64_t first = 0;
    std::uint64_t got = 0;
    // Strong entity tag with its quotes, or empty.
    std::string etag;
    // The server ignored the suffix of a longer object and nothing was stored.
    bool suffix_ignored = false;
};

// One ranged GET on the calling thread, with the retries, corrections and
// deadline of a read. The destination holds range.length bytes. An absolute
// range may reach past the object, and a suffix may be longer than it.
[[nodiscard]] std::expected<RangeReply, Failure>
get_range(const Locator& locator, CURL* easy, CURLSH* share, RequestBuilder& request_builder,
          const ClientOptions& options, const ByteRange& range, std::span<std::byte> destination,
          std::string_view if_match = {});

} // namespace karu::transport

#endif
