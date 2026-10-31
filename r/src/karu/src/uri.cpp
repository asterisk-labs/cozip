#include "uri.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace karu {
namespace {

constexpr std::string_view VSI_SUBFILE = "/vsisubfile/";
constexpr std::string_view BYTES_FRAGMENT = "#bytes=";

std::expected<std::uint64_t, ResolveError> parse_u64(std::string_view text) {
    if (text.empty())
        return std::unexpected("expected a number");
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::unexpected("'" + std::string(text) + "' is not a byte count");
    return value;
}

std::expected<std::pair<std::string_view, std::string_view>, ResolveError>
split_object(std::string_view text, std::string_view grammar) {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= text.size())
        return std::unexpected("expected " + std::string(grammar) + " in '" + std::string(text) +
                               "'");
    return std::pair{text.substr(0, slash), text.substr(slash + 1)};
}

std::expected<void, ResolveError> take_bytes_fragment(std::string& uri, std::uint64_t& offset,
                                                      std::uint64_t& length) {
    const std::size_t hash = uri.rfind('#');
    if (hash == std::string::npos)
        return {};
    std::string_view fragment(uri.data() + hash, uri.size() - hash);
    if (!fragment.starts_with(BYTES_FRAGMENT))
        return {};

    fragment.remove_prefix(BYTES_FRAGMENT.size());
    const std::size_t dash = fragment.find('-');
    if (dash == std::string_view::npos)
        return std::unexpected("#bytes= wants FIRST-LAST");
    auto first = parse_u64(fragment.substr(0, dash));
    if (!first)
        return std::unexpected(first.error());
    const std::string_view last_text = fragment.substr(dash + 1);
    if (last_text.empty()) {
        offset = *first;
        length = TO_END;
    } else {
        auto last = parse_u64(last_text);
        if (!last)
            return std::unexpected(last.error());
        if (*last < *first)
            return std::unexpected("#bytes= ends before it starts");
        const std::uint64_t distance = *last - *first;
        if (distance == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected("#bytes= range is too large");
        offset = *first;
        length = distance + 1;
    }
    uri.resize(hash);
    return {};
}

std::expected<Resolved, ResolveError> terminal(std::string uri) {
    std::uint64_t fragment_offset = 0;
    std::uint64_t fragment_length = TO_END;
    if (auto parsed = take_bytes_fragment(uri, fragment_offset, fragment_length); !parsed)
        return std::unexpected(parsed.error());

    Resolved result;
    const std::string_view input(uri);
    auto finish = [&](Resolved value) -> std::expected<Resolved, ResolveError> {
        if (fragment_offset != 0 || fragment_length != TO_END) {
            if (auto applied = apply_window(value, fragment_offset, fragment_length); !applied) {
                return std::unexpected(applied.error());
            }
        }
        return value;
    };

    if (input.starts_with("http://") || input.starts_with("https://")) {
        const std::size_t authority = input.find("://") + 3;
        const std::size_t end = input.find_first_of("/?#", authority);
        if (authority == input.size() || end == authority)
            return std::unexpected("HTTP URL has no authority");
        result.backend = Backend::Http;
        result.target = uri;
        result.canonical_uri = uri;
        return finish(std::move(result));
    }

    auto cloud = [&](Backend backend, std::string_view rest, std::string_view grammar,
                     std::string_view vsi,
                     bool dfs = false) -> std::expected<Resolved, ResolveError> {
        auto pieces = split_object(rest, grammar);
        if (!pieces)
            return std::unexpected(pieces.error());
        result.backend = backend;
        result.container = std::string(pieces->first);
        result.key = std::string(pieces->second);
        result.azure_dfs = dfs;
        result.canonical_uri = std::string(vsi) + result.container + "/" + result.key;
        return finish(std::move(result));
    };

    if (input.starts_with("s3://"))
        return cloud(Backend::S3, input.substr(5), "s3://bucket/key", "/vsis3/");
    if (input.starts_with("gs://"))
        return cloud(Backend::Gcs, input.substr(5), "gs://bucket/key", "/vsigs/");
    if (input.starts_with("az://"))
        return cloud(Backend::Azure, input.substr(5), "az://container/blob", "/vsiaz/");
    if (input.starts_with("abfs://"))
        return cloud(Backend::Azure, input.substr(7), "abfs://container/blob", "/vsiadls/", true);
    if (input.starts_with("source://")) {
        const std::string_view rest = input.substr(9);
        auto account = split_object(rest, "source://account/product/key");
        if (!account)
            return std::unexpected(account.error());
        auto product = split_object(account->second, "source://account/product/key");
        if (!product)
            return std::unexpected(product.error());
        result.backend = Backend::Source;
        result.container = std::string(account->first);
        result.key = std::string(account->second);
        result.canonical_uri = "/vsisource/" + result.container + "/" + result.key;
        return finish(std::move(result));
    }

    if (input.starts_with("hf://")) {
        std::string_view rest = input.substr(5);
        std::string kind;
        for (const std::string_view prefix : {"datasets/", "spaces/"}) {
            if (rest.starts_with(prefix)) {
                kind = std::string(prefix);
                rest.remove_prefix(prefix.size());
                break;
            }
        }
        auto owner = split_object(rest, "hf://owner/name/path");
        if (!owner)
            return std::unexpected(owner.error());
        auto repository = split_object(owner->second, "hf://owner/name/path");
        if (!repository)
            return std::unexpected(repository.error());
        std::string_view name = repository->first;
        std::string revision = "main";
        if (const std::size_t at = name.find('@'); at != std::string_view::npos) {
            revision = std::string(name.substr(at + 1));
            name = name.substr(0, at);
            if (name.empty() || revision.empty())
                return std::unexpected("hf:// has an empty repository or revision");
        }
        result.backend = Backend::HuggingFace;
        result.canonical_uri = "/vsihf/" + std::string(input.substr(5));
        result.target = "/" + kind + std::string(owner->first) + "/" + std::string(name) +
                        "/resolve/" + encode_path(revision) + "/" + encode_path(repository->second);
        return finish(std::move(result));
    }

    if (input.starts_with("file://")) {
        result.backend = Backend::File;
        result.target = std::string(input.substr(7));
        result.canonical_uri = "file://" + result.target;
        if (result.target.empty())
            return std::unexpected("file:// has no path");
        return finish(std::move(result));
    }
    if (input.find("://") != std::string_view::npos)
        return std::unexpected("unknown scheme in '" + uri + "'");
    if (input.empty())
        return std::unexpected("empty path");
    result.backend = Backend::File;
    result.target = uri;
    result.canonical_uri = uri;
    return finish(std::move(result));
}

} // namespace

std::string encode_path(std::string_view path) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(path.size());
    for (const unsigned char c : path) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
                          c == '/';
        if (safe) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(HEX[c >> 4]);
            result.push_back(HEX[c & 0x0f]);
        }
    }
    return result;
}

std::expected<void, ResolveError> apply_window(Resolved& resolved, std::uint64_t offset,
                                               std::uint64_t length) {
    // Nested subfiles are clipped to the parent window, then expressed as one
    // offset into the terminal object.
    if (resolved.window_offset > std::numeric_limits<std::uint64_t>::max() - offset)
        return std::unexpected("window offset overflows");
    std::uint64_t composed = length;
    if (resolved.window_length != TO_END) {
        if (offset > resolved.window_length)
            return std::unexpected("window starts past the end of its parent");
        composed = std::min(length, resolved.window_length - offset);
    }
    resolved.window_offset += offset;
    resolved.window_length = composed;
    return {};
}

namespace {

constexpr std::size_t MAX_SUBFILE_DEPTH = 16;

std::expected<Resolved, ResolveError> resolve_impl(std::string_view uri, std::size_t depth) {
    if (uri.empty())
        return std::unexpected("empty URI");
    if (uri.starts_with(VSI_SUBFILE)) {
        if (depth == MAX_SUBFILE_DEPTH)
            return std::unexpected("/vsisubfile/ nesting exceeds 16 levels");
        const std::string_view rest = uri.substr(VSI_SUBFILE.size());
        const std::size_t comma = rest.find(',');
        if (comma == std::string_view::npos)
            return std::unexpected("/vsisubfile/ wants OFFSET[_LENGTH],<uri>");
        const std::string_view spec = rest.substr(0, comma);
        const std::string_view inner = rest.substr(comma + 1);
        if (inner.empty())
            return std::unexpected("/vsisubfile/ has no inner URI");

        std::uint64_t offset = 0;
        std::uint64_t length = TO_END;
        const std::size_t underscore = spec.find('_');
        auto parsed_offset = parse_u64(spec.substr(0, underscore));
        if (!parsed_offset)
            return std::unexpected(parsed_offset.error());
        offset = *parsed_offset;
        if (underscore != std::string_view::npos) {
            const std::string_view length_text = spec.substr(underscore + 1);
            if (!length_text.empty() && length_text.front() == '-') {
                if (!parse_u64(length_text.substr(1)))
                    return std::unexpected("negative subfile length is malformed");
            } else {
                auto parsed_length = parse_u64(length_text);
                if (!parsed_length)
                    return std::unexpected(parsed_length.error());
                length = *parsed_length;
            }
        }
        auto child = resolve_impl(inner, depth + 1);
        if (!child)
            return child;
        if (auto applied = apply_window(*child, offset, length); !applied)
            return std::unexpected(applied.error());
        return child;
    }

    struct Alias {
        std::string_view prefix;
        std::string_view scheme;
    };
    static constexpr Alias ALIASES[] = {
        {"/vsicurl/", ""},
        {"/vsis3/", "s3://"},
        {"/vsigs/", "gs://"},
        {"/vsiaz/", "az://"},
        {"/vsiadls/", "abfs://"},
        {"/vsihf/", "hf://"},
        {"/vsisource/", "source://"},
    };
    for (const auto& [prefix, scheme] : ALIASES) {
        if (!uri.starts_with(prefix))
            continue;
        const std::string_view rest = uri.substr(prefix.size());
        if (rest.empty())
            return std::unexpected(std::string(prefix) + " has no target");
        if (prefix == "/vsicurl/" && !rest.starts_with("http://") &&
            !rest.starts_with("https://")) {
            return std::unexpected("/vsicurl/ target must be an HTTP URL");
        }
        return terminal(std::string(scheme) + std::string(rest));
    }

    if (uri.starts_with("/vsi")) {
        const std::size_t end = uri.find('/', 4);
        const std::string handler(
            uri.substr(0, end == std::string_view::npos ? uri.size() : end + 1));
        return std::unexpected(
            ResolveError{ResolveErrorCode::Unsupported,
                         "unsupported virtual filesystem '" + handler +
                             "'; Karu supports /vsisubfile/, /vsicurl/, /vsis3/, /vsigs/, "
                             "/vsiaz/, /vsiadls/, /vsihf/ and /vsisource/ "
                             "(no *_streaming aliases)"});
    }
    return terminal(std::string(uri));
}

} // namespace

std::expected<Resolved, ResolveError> resolve(std::string_view uri) {
    return resolve_impl(uri, 0);
}

} // namespace karu
