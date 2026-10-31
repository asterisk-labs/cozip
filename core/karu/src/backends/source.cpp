#include "contract.hpp"
#include "s3_signing.hpp"
#include "source_credentials.hpp"
#include "url.hpp"

#include <array>

namespace karu::backends {
namespace {

constexpr std::array<std::string_view, 6> CREDENTIAL_OPTIONS{
    "SOURCE_ACCESS_KEY_ID", "SOURCE_SECRET_ACCESS_KEY",       "SOURCE_SESSION_TOKEN",
    "SOURCE_PROFILE",       "SOURCE_SHARED_CREDENTIALS_FILE", "SOURCE_CONFIG_FILE"};

std::expected<PreparedRequest, RequestError> prepare_source(const RequestContext& request) {
    const std::string& path = request.object.canonical_uri;
    std::string endpoint =
        request.config.option(path, "SOURCE_ENDPOINT", "https://data.source.coop");
    auto normalized = http_endpoint(std::move(endpoint), "SOURCE_ENDPOINT");
    if (!normalized)
        return std::unexpected(normalized.error());
    endpoint = std::move(*normalized);

    const std::string credential_region =
        request.credentials == nullptr ? "" : request.credentials->region;
    const std::string region = request.config.option(
        path, "SOURCE_REGION", credential_region.empty() ? "us-east-1" : credential_region);
    if (!valid_aws_region(region)) {
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, path + ": Source region contains invalid characters"});
    }
    if (request.credentials != nullptr && (request.credentials->access_key_id.empty() ||
                                           request.credentials->secret_access_key.empty())) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, path + ": Source credentials require an access key and secret"});
    }

    const S3GetRequest target{.endpoint = std::move(endpoint),
                              .endpoint_option = "SOURCE_ENDPOINT",
                              .bucket = request.object.container,
                              .key = request.object.key,
                              .region = region,
                              .virtual_hosting = false,
                              .request_payer = {}};
    return prepare_s3_get(target, request.credentials, request.range, request.if_match,
                          request.signing_time);
}

} // namespace

const CloudProvider& source_provider() noexcept {
    static const CloudProvider provider{Backend::Source,
                                        KARU_CREDENTIALS_SOURCE,
                                        "SOURCE_NO_SIGN_REQUEST",
                                        true,
                                        CREDENTIAL_OPTIONS,
                                        load_source_credentials,
                                        prepare_source};
    return provider;
}

} // namespace karu::backends
