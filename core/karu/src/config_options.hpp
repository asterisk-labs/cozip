#ifndef KARU_CONFIG_OPTIONS_HPP
#define KARU_CONFIG_OPTIONS_HPP

#include <array>

namespace karu::config_options {

// The order inside a group defines environment-alias precedence: the first
// spelling wins. Explicit API values and path-scoped values are separate,
// higher-precedence layers.
inline constexpr std::array CORE{
    "KARU_CONCURRENCY",          "KARU_IO_THREADS",      "KARU_COALESCE_GAP",
    "KARU_COALESCE_LIMIT",       "KARU_COALESCE_PARTS",  "KARU_COALESCE_AMPLIFICATION",
    "KARU_RANGE_FALLBACK_LIMIT", "KARU_MAX_ATTEMPTS",    "KARU_MAX_RETRIES",
    "KARU_REQUEST_TIMEOUT",      "KARU_CONNECT_TIMEOUT", "KARU_LOW_SPEED_TIME",
    "KARU_LOW_SPEED_LIMIT",
};

inline constexpr std::array S3{
    "AWS_NO_SIGN_REQUEST",
    "AWS_ACCESS_KEY_ID",
    "AWS_SECRET_ACCESS_KEY",
    "AWS_SESSION_TOKEN",
    "AWS_REGION",
    "AWS_DEFAULT_REGION",
    "AWS_PROFILE",
    "AWS_DEFAULT_PROFILE",
    "AWS_SHARED_CREDENTIALS_FILE",
    "AWS_CONFIG_FILE",
    "AWS_S3_ENDPOINT",
    "AWS_ENDPOINT_URL_S3",
    "AWS_ENDPOINT_URL",
    "AWS_HTTPS",
    "AWS_VIRTUAL_HOSTING",
    "AWS_REQUEST_PAYER",
    "AWS_ROLE_ARN",
    "AWS_STS_ENDPOINT",
    "AWS_WEB_IDENTITY_TOKEN_FILE",
    "AWS_ROLE_SESSION_NAME",
    "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
    "AWS_CONTAINER_CREDENTIALS_FULL_URI",
    "AWS_CONTAINER_AUTHORIZATION_TOKEN",
    "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE",
    "AWS_EC2_METADATA_DISABLED",
    "AWS_METADATA_SERVICE_TIMEOUT",
};

inline constexpr std::array GCS{
    "GCS_NO_SIGN_REQUEST",
    "GCS_HMAC_ACCESS_KEY_ID",
    "GCS_HMAC_SECRET_ACCESS_KEY",
    "GCS_ACCESS_TOKEN",
    "GCS_REFRESH_TOKEN",
    "GCS_CLIENT_ID",
    "GCS_CLIENT_SECRET",
    "GCS_PRIVATE_KEY",
    "GCS_PRIVATE_KEY_FILE",
    "GCS_CLIENT_EMAIL",
    "GCS_SCOPE",
    "GCS_USER_PROJECT",
    "GCS_ENDPOINT",
    "GCS_METADATA_ENDPOINT",
    "GCS_METADATA_DISABLED",
    "GOOGLE_APPLICATION_CREDENTIALS",
    "CLOUDSDK_CONFIG",
};

inline constexpr std::array AZURE{
    "AZURE_NO_SIGN_REQUEST",
    "AZURE_STORAGE_CONNECTION_STRING",
    "AZURE_STORAGE_ACCOUNT",
    "AZURE_STORAGE_ACCESS_KEY",
    "AZURE_STORAGE_SAS_TOKEN",
    "AZURE_STORAGE_ACCESS_TOKEN",
    "AZURE_STORAGE_ENDPOINT",
    "AZURE_TENANT_ID",
    "AZURE_CLIENT_ID",
    "AZURE_CLIENT_SECRET",
    "AZURE_FEDERATED_TOKEN_FILE",
    "AZURE_AUTHORITY_HOST",
    "AZURE_STORAGE_SCOPE",
    "AZURE_STORAGE_RESOURCE",
    "AZURE_IMDS_OBJECT_ID",
    "AZURE_IMDS_CLIENT_ID",
    "AZURE_IMDS_MSI_RES_ID",
    "IDENTITY_ENDPOINT",
    "IDENTITY_HEADER",
    "IMDS_ENDPOINT",
};

inline constexpr std::array HUGGING_FACE{
    "HF_ENDPOINT", "HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "HUGGING_FACE_HUB_TOKEN",
};

inline constexpr std::array SOURCE{
    "SOURCE_NO_SIGN_REQUEST", "SOURCE_ACCESS_KEY_ID", "SOURCE_SECRET_ACCESS_KEY",
    "SOURCE_SESSION_TOKEN",   "SOURCE_PROFILE",       "SOURCE_SHARED_CREDENTIALS_FILE",
    "SOURCE_CONFIG_FILE",     "SOURCE_REGION",        "SOURCE_ENDPOINT",
    "SOURCE_PROXY_URL",
};

inline constexpr std::array HTTP{
    "KARU_HTTP_HEADERS",    "KARU_HTTP_VERSION",
    "KARU_HTTP_CA_BUNDLE",  "CURL_CA_BUNDLE",
    "SSL_CERT_FILE",        "KARU_HTTP_CA_PATH",
    "KARU_HTTP_PROXY",      "KARU_HTTP_PROXY_CREDENTIALS",
    "KARU_HTTP_USER_AGENT",
};

template <typename Visitor> void for_each_environment(Visitor&& visitor) {
    const auto visit = [&](const auto& group) {
        for (const char* name : group)
            visitor(name);
    };
    visit(CORE);
    visit(S3);
    visit(GCS);
    visit(AZURE);
    visit(HUGGING_FACE);
    visit(SOURCE);
    visit(HTTP);
}

} // namespace karu::config_options

#endif
