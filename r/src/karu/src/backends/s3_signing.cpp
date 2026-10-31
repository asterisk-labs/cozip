#include "s3_signing.hpp"

#include "../uri.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <openssl/crypto.h>
#include <ranges>
#include <span>

namespace karu::backends {
namespace {

std::string virtual_host_url(const UrlParts& endpoint, std::string_view bucket,
                             std::string_view key) {
    return endpoint.scheme + "://" + std::string(bucket) + "." + endpoint.authority +
           (endpoint.path == "/" ? "/" : without_trailing_slash(endpoint.path) + "/") +
           encode_path(key);
}

std::string signed_host(const UrlParts& url) {
    const std::string_view default_port = url.scheme == "https" ? ":443" : ":80";
    if (url.authority.ends_with(default_port))
        return url.authority.substr(0, url.authority.size() - default_port.size());
    return url.authority;
}

std::string hex(std::span<const unsigned char> bytes) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = DIGITS[bytes[index] >> 4];
        result[index * 2 + 1] = DIGITS[bytes[index] & 0x0f];
    }
    return result;
}

// For S3, the derived key changes only with the secret, date or region.
// Cache the last key per thread to avoid four HMACs on each request.
struct SigningKeyCache {
    std::string secret;
    std::string date;
    std::string region;
    Sha256Digest key{};
    bool valid = false;

    SigningKeyCache() = default;
    SigningKeyCache(const SigningKeyCache&) = delete;
    SigningKeyCache& operator=(const SigningKeyCache&) = delete;
    ~SigningKeyCache() { forget(); }

    void forget() noexcept {
        OPENSSL_cleanse(secret.data(), secret.size());
        OPENSSL_cleanse(key.data(), key.size());
        secret.clear();
        valid = false;
    }
};

std::expected<Sha256Digest, RequestError>
signing_key(std::string_view secret, std::string_view date, std::string_view region) {
    thread_local SigningKeyCache cache;
    if (cache.valid && cache.secret == secret && cache.date == date && cache.region == region)
        return cache.key;

    std::string initial = "AWS4" + std::string(secret);
    auto date_key = hmac_sha256(
        std::span(reinterpret_cast<const unsigned char*>(initial.data()), initial.size()), date);
    OPENSSL_cleanse(initial.data(), initial.size());
    if (!date_key)
        return std::unexpected(date_key.error());
    auto region_key = hmac_sha256(*date_key, region);
    if (!region_key)
        return std::unexpected(region_key.error());
    auto service_key = hmac_sha256(*region_key, "s3");
    if (!service_key)
        return std::unexpected(service_key.error());
    auto key = hmac_sha256(*service_key, "aws4_request");
    if (!key)
        return std::unexpected(key.error());

    cache.forget();
    cache.secret = secret;
    cache.date = date;
    cache.region = region;
    cache.key = *key;
    cache.valid = true;
    return *key;
}

} // namespace

bool valid_aws_region(std::string_view region) noexcept {
    return !region.empty() && std::ranges::all_of(region, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-';
    });
}

std::expected<PreparedRequest, RequestError>
prepare_s3_get(const S3GetRequest& target, const ProviderCredentials* credentials,
               std::string_view range, std::string_view if_match, std::time_t signing_time) {
    auto endpoint = split_url(target.endpoint);
    if (!endpoint)
        return std::unexpected(endpoint.error());
    if (!endpoint->query.empty())
        return std::unexpected(RequestError{KARU_ERR_CONFIG, std::string(target.endpoint_option) +
                                                                 " cannot contain a query"});

    std::string url = target.virtual_hosting
                          ? virtual_host_url(*endpoint, target.bucket, target.key)
                          : append_object(target.endpoint, target.bucket, target.key);
    if (credentials == nullptr) {
        PreparedRequest request{std::move(url), {}};
        if (!if_match.empty())
            request.headers.emplace_back("If-Match", if_match);
        return request;
    }

    auto url_parts = split_url(url);
    if (!url_parts)
        return std::unexpected(url_parts.error());
    const std::string timestamp = date_utc(signing_time, "%Y%m%dT%H%M%SZ");
    const std::string date = timestamp.substr(0, 8);
    const std::string payload = "UNSIGNED-PAYLOAD";

    std::vector<Header> signed_headers{{"host", signed_host(*url_parts)},
                                       {"range", std::string(range)},
                                       {"x-amz-content-sha256", payload},
                                       {"x-amz-date", timestamp}};
    if (!if_match.empty())
        signed_headers.emplace_back("if-match", if_match);
    if (!credentials->session_token.empty())
        signed_headers.emplace_back("x-amz-security-token", credentials->session_token);
    if (lower(std::string(target.request_payer)) == "requester")
        signed_headers.emplace_back("x-amz-request-payer", "requester");
    std::ranges::sort(signed_headers, {}, &Header::first);

    std::string canonical_headers;
    std::string signed_names;
    for (const auto& [name, value] : signed_headers) {
        canonical_headers += lower(name) + ":" + trim(value) + "\n";
        if (!signed_names.empty())
            signed_names.push_back(';');
        signed_names += lower(name);
    }
    const std::string canonical_request = "GET\n" + url_parts->path + "\n" + url_parts->query +
                                          "\n" + canonical_headers + "\n" + signed_names + "\n" +
                                          payload;
    const std::string scope = date + "/" + std::string(target.region) + "/s3/aws4_request";
    auto request_hash = sha256(canonical_request);
    if (!request_hash)
        return std::unexpected(request_hash.error());
    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" + hex(*request_hash);

    auto key = signing_key(credentials->secret_access_key, date, target.region);
    if (!key)
        return std::unexpected(key.error());
    auto signed_request = hmac_sha256(*key, string_to_sign);
    if (!signed_request)
        return std::unexpected(signed_request.error());
    const std::string signature = hex(*signed_request);

    std::vector<Header> headers;
    headers.reserve(signed_headers.size() + 1);
    for (const auto& [name, value] : signed_headers) {
        if (name != "host" && name != "range")
            headers.emplace_back(name, value);
    }
    headers.emplace_back("Authorization",
                         "AWS4-HMAC-SHA256 Credential=" + credentials->access_key_id + "/" + scope +
                             ", SignedHeaders=" + signed_names + ", Signature=" + signature);
    PreparedRequest request{std::move(url), std::move(headers)};
    request.http.follow_redirects = false;
    return request;
}

} // namespace karu::backends
