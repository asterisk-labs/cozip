#ifndef KARU_REQUEST_HPP
#define KARU_REQUEST_HPP

#include "karu/karu.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace karu {

enum class HttpVersion {
    Automatic,
    Http1_1,
    Http2Tls,
    Http2PriorKnowledge,
};

struct HttpRequestOptions {
    HttpVersion version = HttpVersion::Http1_1;
    // Cloud backends disable redirects when credentials are bound to the
    // original request or URL.
    bool follow_redirects = true;
    // User-configured headers are scoped to the original HTTP origin.
    bool same_origin_redirects_only = false;
    std::string ca_bundle;
    std::string ca_path;
    std::string proxy;
    std::string proxy_user_password;
    std::string user_agent;

    bool operator==(const HttpRequestOptions&) const = default;
};

struct PreparedRequest {
    PreparedRequest(std::string request_url,
                    std::vector<std::pair<std::string, std::string>> request_headers)
        : url(std::move(request_url)), headers(std::move(request_headers)) {}

    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string range;
    HttpRequestOptions http;
    std::string routing_region;
};

struct RequestError {
    karu_status status = KARU_ERR_CREDENTIALS;
    std::string message;
};

struct ProviderCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::string bearer_token;
    std::string sas_token;
    std::string account_name;
    std::string region;
    std::string cache_prefix;
    std::int64_t expires_at = 0;
};

struct ResolvedCredentials {
    std::optional<ProviderCredentials> value;
};

} // namespace karu

#endif
