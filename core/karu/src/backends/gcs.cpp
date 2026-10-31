#include "../text.hpp"
#include "contract.hpp"
#include "crypto.hpp"
#include "gcs_credentials.hpp"
#include "url.hpp"

#include <array>
#include <ctime>
#include <openssl/evp.h>
#include <span>

namespace karu::backends {
namespace {

constexpr std::array<std::string_view, 14> CREDENTIAL_OPTIONS{"GCS_HMAC_ACCESS_KEY_ID",
                                                              "GCS_HMAC_SECRET_ACCESS_KEY",
                                                              "GCS_ACCESS_TOKEN",
                                                              "GCS_REFRESH_TOKEN",
                                                              "GCS_CLIENT_ID",
                                                              "GCS_CLIENT_SECRET",
                                                              "GCS_PRIVATE_KEY",
                                                              "GCS_PRIVATE_KEY_FILE",
                                                              "GCS_CLIENT_EMAIL",
                                                              "GCS_SCOPE",
                                                              "GOOGLE_APPLICATION_CREDENTIALS",
                                                              "CLOUDSDK_CONFIG",
                                                              "GCS_METADATA_DISABLED",
                                                              "GCS_METADATA_ENDPOINT"};

std::expected<PreparedRequest, RequestError>
prepare_gcs(const ConfigSnapshot& config, const Resolved& object, const ProviderCredentials* custom,
            std::string_view if_match, std::time_t signing_time) {
    const std::string& path = object.canonical_uri;
    std::string endpoint = config.option(path, "GCS_ENDPOINT", "https://storage.googleapis.com");
    auto normalized_endpoint = http_endpoint(std::move(endpoint), "GCS_ENDPOINT");
    if (!normalized_endpoint)
        return std::unexpected(normalized_endpoint.error());
    endpoint = std::move(*normalized_endpoint);
    auto endpoint_parts = split_url(endpoint);
    if (!endpoint_parts)
        return std::unexpected(endpoint_parts.error());
    if (!endpoint_parts->query.empty())
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "GCS_ENDPOINT cannot contain a query"});
    std::string url = append_object(endpoint, object.container, object.key);
    // Keep GCS from decompressing gzip objects and ignoring Range. A manual
    // Accept-Encoding header leaves libcurl's automatic decoding disabled.
    std::vector<Header> headers{{"Accept-Encoding", "gzip"}};
    if (!if_match.empty())
        headers.emplace_back("If-Match", if_match);
    const std::string user_project = config.option(path, "GCS_USER_PROJECT");
    if (!user_project.empty())
        headers.emplace_back("x-goog-user-project", user_project);
    if (option_is_true(config.option(path, "GCS_NO_SIGN_REQUEST", "NO")))
        return PreparedRequest{std::move(url), std::move(headers)};

    ProviderCredentials credentials;
    if (custom != nullptr) {
        credentials = *custom;
    } else {
        credentials.access_key_id = config.option(path, "GCS_HMAC_ACCESS_KEY_ID");
        credentials.secret_access_key = config.option(path, "GCS_HMAC_SECRET_ACCESS_KEY");
        credentials.bearer_token = config.option(path, "GCS_ACCESS_TOKEN");
    }
    if (!credentials.bearer_token.empty()) {
        headers.emplace_back("Authorization", "Bearer " + credentials.bearer_token);
        PreparedRequest request{std::move(url), std::move(headers)};
        request.http.follow_redirects = false;
        return request;
    }
    if (!credentials.access_key_id.empty() && !credentials.secret_access_key.empty()) {
        const std::string date = rfc7231_date(signing_time);
        const std::string canonical_headers =
            user_project.empty() ? "" : "x-goog-user-project:" + user_project + "\n";
        const std::string canonical = "GET\n\n\n" + date + "\n" + canonical_headers + "/" +
                                      object.container + "/" + encode_path(object.key);
        const auto key =
            std::span(reinterpret_cast<const unsigned char*>(credentials.secret_access_key.data()),
                      credentials.secret_access_key.size());
        auto signature = hmac(EVP_sha1(), key, canonical);
        if (!signature)
            return std::unexpected(signature.error());
        headers.emplace_back("Date", date);
        headers.emplace_back("Authorization",
                             "GOOG1 " + credentials.access_key_id + ":" + base64(*signature));
        PreparedRequest request{std::move(url), std::move(headers)};
        request.http.follow_redirects = false;
        return request;
    }
    return std::unexpected(
        RequestError{KARU_ERR_CREDENTIALS,
                     path + ": no GCS credentials; set GCS_ACCESS_TOKEN, configure "
                            "GCS_HMAC_ACCESS_KEY_ID/GCS_HMAC_SECRET_ACCESS_KEY, install a custom "
                            "provider, or set GCS_NO_SIGN_REQUEST=YES"});
}

} // namespace

const CloudProvider& gcs_provider() noexcept {
    static const CloudProvider provider{Backend::Gcs,
                                        KARU_CREDENTIALS_GCS,
                                        "GCS_NO_SIGN_REQUEST",
                                        false,
                                        CREDENTIAL_OPTIONS,
                                        load_gcs_credentials,
                                        [](const RequestContext& request) {
                                            return prepare_gcs(
                                                request.config, request.object, request.credentials,
                                                request.if_match, request.signing_time);
                                        }};
    return provider;
}

} // namespace karu::backends
