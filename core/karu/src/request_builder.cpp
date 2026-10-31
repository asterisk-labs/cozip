#include "request_builder.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"

#include <ctime>

namespace karu {
namespace {

void apply_http_options(const ConfigSnapshot& config, PreparedRequest& request) {
    const bool follow_redirects = request.http.follow_redirects;
    request.http = config.http_options();
    request.http.follow_redirects = follow_redirects;
    request.http.same_origin_redirects_only = config.has_configured_headers();
}

} // namespace

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare(const Locator& locator, std::uint64_t first, std::uint64_t length,
                        std::string_view region_hint, std::string_view if_match) {
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});
    auto credentials = resolve_credentials(locator);
    if (!credentials)
        return std::unexpected(credentials.error());
    return materialize(locator, *credentials, first, length, region_hint, if_match);
}

std::expected<ResolvedCredentials, RequestError>
RequestBuilder::resolve_credentials(const Locator& locator) {
    const Resolved& object = locator.resolved;
    if (object.backend == Backend::Http || object.backend == Backend::HuggingFace)
        return ResolvedCredentials{};

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    const auto custom_provider = config_.provider(provider->credentials_kind);
    bool credentials_configured = custom_provider != nullptr;
    if (provider->anonymous_by_default && !credentials_configured) {
        for (std::string_view option : provider->credential_options) {
            if (config_.has_option(object.canonical_uri, option)) {
                credentials_configured = true;
                break;
            }
        }
    }

    const bool no_sign =
        config_.has_option(object.canonical_uri, provider->no_sign_option)
            ? option_is_true(config_.option(object.canonical_uri, provider->no_sign_option))
            : provider->anonymous_by_default && !credentials_configured;
    if (no_sign)
        return ResolvedCredentials{};

    auto loaded = custom_provider ? credentials_.custom(config_, provider->credentials_kind,
                                                        object.canonical_uri)
                                  : credentials_.native(config_, *provider, object.canonical_uri);
    if (!loaded)
        return std::unexpected(loaded.error());
    return ResolvedCredentials{std::move(*loaded)};
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::materialize(const Locator& locator, const ResolvedCredentials& credentials,
                            std::uint64_t first, std::uint64_t length, std::string_view region_hint,
                            std::string_view if_match) {
    if (length == 0 || first > UINT64_MAX - (length - 1))
        return std::unexpected(RequestError{KARU_ERR_RANGE, "HTTP range overflows"});
    return materialize_range(locator, credentials, backends::range_header(first, length),
                             region_hint, if_match);
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::prepare_suffix(const Locator& locator, std::uint64_t length,
                               std::string_view region_hint, std::string_view if_match) {
    if (length == 0)
        return std::unexpected(RequestError{KARU_ERR_RANGE, "empty suffix range"});
    auto credentials = resolve_credentials(locator);
    if (!credentials)
        return std::unexpected(credentials.error());
    return materialize_range(locator, *credentials, backends::suffix_range_header(length),
                             region_hint, if_match);
}

std::expected<PreparedRequest, RequestError>
RequestBuilder::materialize_range(const Locator& locator, const ResolvedCredentials& credentials,
                                  std::string range, std::string_view region_hint,
                                  std::string_view if_match) {
    const Resolved& object = locator.resolved;
    if (object.backend == Backend::Http || object.backend == Backend::HuggingFace) {
        auto prepared = object.backend == Backend::Http
                            ? backends::prepare_http(config_, object, if_match)
                            : backends::prepare_hugging_face(config_, object, if_match);
        if (prepared) {
            prepared->range = std::move(range);
            apply_http_options(config_, *prepared);
        }
        return prepared;
    }

    const backends::CloudProvider* provider = backends::cloud_provider(object.backend);
    if (provider == nullptr)
        return std::unexpected(RequestError{KARU_ERR_UNSUPPORTED, "unsupported request backend"});

    // The signature must see the final range and routing region. Retries call
    // materialize again after either one changes.
    backends::RequestContext request{config_,
                                     object,
                                     credentials.value ? &*credentials.value : nullptr,
                                     range,
                                     region_hint,
                                     if_match,
                                     std::time(nullptr)};
    auto prepared = provider->prepare_request(request);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto headers = backends::add_configured_headers(config_, object.canonical_uri,
                                                        prepared->headers, true);
        !headers) {
        return std::unexpected(headers.error());
    }
    prepared->range = std::move(range);
    apply_http_options(config_, *prepared);
    return prepared;
}

void RequestBuilder::invalidate_credentials(const Locator& locator) {
    const backends::CloudProvider* provider = backends::cloud_provider(locator.resolved.backend);
    if (provider == nullptr)
        return;
    const bool custom = config_.provider(provider->credentials_kind) != nullptr;
    credentials_.invalidate(config_, *provider, locator.resolved.canonical_uri, custom);
}

} // namespace karu
