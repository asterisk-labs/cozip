// Transport-independent URI parsing. No endpoint or credential is read here.
#ifndef KARU_URI_HPP
#define KARU_URI_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace karu {

inline constexpr std::uint64_t TO_END = UINT64_MAX;

enum class Backend { File, Http, S3, Gcs, Azure, HuggingFace, Source };

struct Resolved {
    Backend backend = Backend::File;
    std::string canonical_uri;
    std::string target;    // local path, raw HTTP URL, or HF endpoint path
    std::string container; // S3/GCS bucket, Azure container, or Source account
    std::string key;       // exact object key; empty segments are significant
    bool azure_dfs = false;
    std::uint64_t window_offset = 0;
    std::uint64_t window_length = TO_END;
};

enum class ResolveErrorCode { Malformed, Unsupported };

struct ResolveError {
    ResolveError(const char* text) : message(text) {}
    ResolveError(std::string text) : message(std::move(text)) {}
    ResolveError(ResolveErrorCode error_code, std::string text)
        : code(error_code), message(std::move(text)) {}

    ResolveErrorCode code = ResolveErrorCode::Malformed;
    std::string message;
};

[[nodiscard]] std::expected<Resolved, ResolveError> resolve(std::string_view uri);
[[nodiscard]] std::expected<void, ResolveError>
apply_window(Resolved& resolved, std::uint64_t offset, std::uint64_t length);
[[nodiscard]] std::string encode_path(std::string_view path);

} // namespace karu

#endif
