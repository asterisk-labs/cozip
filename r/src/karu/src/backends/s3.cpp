#include "contract.hpp"
#include "s3_credentials.hpp"
#include "s3_signing.hpp"
#include "url.hpp"

#include <array>

namespace karu::backends {
namespace {

constexpr std::array<std::string_view, 16> CREDENTIAL_OPTIONS{
    "AWS_ACCESS_KEY_ID",
    "AWS_SECRET_ACCESS_KEY",
    "AWS_SESSION_TOKEN",
    "AWS_PROFILE",
    "AWS_SHARED_CREDENTIALS_FILE",
    "AWS_CONFIG_FILE",
    "AWS_ROLE_ARN",
    "AWS_STS_ENDPOINT",
    "AWS_WEB_IDENTITY_TOKEN_FILE",
    "AWS_ROLE_SESSION_NAME",
    "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
    "AWS_CONTAINER_CREDENTIALS_FULL_URI",
    "AWS_CONTAINER_AUTHORIZATION_TOKEN",
    "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE",
    "AWS_EC2_METADATA_DISABLED",
    "AWS_METADATA_SERVICE_TIMEOUT"};

std::expected<PreparedRequest, RequestError> prepare_aws(const RequestContext& request) {
    const std::string& path = request.object.canonical_uri;
    const std::string credential_region =
        request.credentials == nullptr ? "" : request.credentials->region;
    const std::string region =
        request.region_hint.empty()
            ? request.config.option(path, "AWS_REGION",
                                    credential_region.empty() ? "us-east-1" : credential_region)
            : std::string(request.region_hint);
    if (!valid_aws_region(region)) {
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, path + ": AWS region contains invalid characters"});
    }

    std::string endpoint = request.config.option(path, "AWS_S3_ENDPOINT");
    const bool default_endpoint = endpoint.empty();
    if (default_endpoint)
        endpoint = "s3." + region + ".amazonaws.com";
    auto normalized =
        http_endpoint(std::move(endpoint), "AWS_S3_ENDPOINT",
                      option_is_true(request.config.option(path, "AWS_HTTPS", "YES")));
    if (!normalized)
        return std::unexpected(normalized.error());
    endpoint = std::move(*normalized);

    const bool no_sign = option_is_true(request.config.option(path, "AWS_NO_SIGN_REQUEST", "NO"));
    ProviderCredentials configured;
    const ProviderCredentials* credentials = nullptr;
    if (!no_sign) {
        if (request.credentials != nullptr) {
            configured = *request.credentials;
        } else {
            configured.access_key_id = request.config.option(path, "AWS_ACCESS_KEY_ID");
            configured.secret_access_key = request.config.option(path, "AWS_SECRET_ACCESS_KEY");
            configured.session_token = request.config.option(path, "AWS_SESSION_TOKEN");
        }
        if (configured.access_key_id.empty() || configured.secret_access_key.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS, path + ": no AWS credentials; set AWS_ACCESS_KEY_ID and "
                                             "AWS_SECRET_ACCESS_KEY, install a custom provider, "
                                             "or set AWS_NO_SIGN_REQUEST=YES for a public object"});
        }
        credentials = &configured;
    }

    const bool virtual_hosting_default =
        default_endpoint && request.object.container.find('.') == std::string::npos;
    const bool virtual_hosting = option_is_true(
        request.config.option(path, "AWS_VIRTUAL_HOSTING", virtual_hosting_default ? "YES" : "NO"));
    const std::string request_payer = request.config.option(path, "AWS_REQUEST_PAYER");
    const S3GetRequest target{.endpoint = std::move(endpoint),
                              .endpoint_option = "AWS_S3_ENDPOINT",
                              .bucket = request.object.container,
                              .key = request.object.key,
                              .region = region,
                              .virtual_hosting = virtual_hosting,
                              .request_payer = request_payer};
    auto prepared =
        prepare_s3_get(target, credentials, request.range, request.if_match, request.signing_time);
    if (prepared)
        prepared->routing_region = region;
    return prepared;
}

} // namespace

const CloudProvider& s3_provider() noexcept {
    static const CloudProvider provider{Backend::S3, KARU_CREDENTIALS_AWS, "AWS_NO_SIGN_REQUEST",
                                        false,       CREDENTIAL_OPTIONS,   load_aws_credentials,
                                        prepare_aws};
    return provider;
}

} // namespace karu::backends
