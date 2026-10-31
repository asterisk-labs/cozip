#include "s3_credentials.hpp"

#include "../text.hpp"
#include "aws_profile.hpp"
#include "credentials.hpp"
#include "url.hpp"

#include <charconv>
#include <ctime>

namespace karu::backends {
namespace {

bool safe_container_endpoint(std::string_view url) {
    auto parts = split_url(url);
    if (!parts)
        return false;
    const std::string scheme = lower(parts->scheme);
    if (scheme == "https")
        return true;
    if (scheme != "http" || parts->authority.find('@') != std::string::npos)
        return false;

    std::string host;
    if (parts->authority.starts_with('[')) {
        const std::size_t closing = parts->authority.find(']');
        if (closing == std::string::npos)
            return false;
        host = parts->authority.substr(1, closing - 1);
        if (closing + 1 < parts->authority.size() && parts->authority[closing + 1] != ':')
            return false;
    } else {
        const std::size_t colon = parts->authority.find(':');
        host = parts->authority.substr(0, colon);
    }
    host = lower(std::move(host));
    return host == "127.0.0.1" || host == "::1" || host == "localhost" || host == "169.254.170.2";
}

std::optional<std::string> xml_value(std::string_view xml, std::string_view name) {
    const std::string open = "<" + std::string(name) + ">";
    const std::string close = "</" + std::string(name) + ">";
    const std::size_t first = xml.find(open);
    if (first == std::string_view::npos)
        return std::nullopt;
    const std::size_t start = first + open.size();
    const std::size_t end = xml.find(close, start);
    return end == std::string_view::npos
               ? std::nullopt
               : std::optional(std::string(xml.substr(start, end - start)));
}

std::expected<ProviderCredentials, RequestError>
aws_web_identity(const std::string& role, const std::string& token_file, std::string session_name,
                 const std::string& endpoint, const HttpRequestOptions& http) {
    auto token = read_text_file(token_file, "AWS web identity token");
    if (!token)
        return std::unexpected(token.error());
    if (session_name.empty())
        session_name = "karu-" + std::to_string(std::time(nullptr));
    const std::string form =
        "Action=AssumeRoleWithWebIdentity&Version=2011-06-15&RoleArn=" + form_encode(role) +
        "&RoleSessionName=" + form_encode(session_name) +
        "&WebIdentityToken=" + form_encode(trim(*token));
    auto response = credential_request(
        "POST", endpoint, form, {{"Content-Type", "application/x-www-form-urlencoded"}}, 5, http);
    if (!response)
        return std::unexpected(response.error());
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         concat("AWS web identity exchange returned HTTP ", response->status)});
    }
    ProviderCredentials result;
    result.access_key_id = xml_value(response->body, "AccessKeyId").value_or("");
    result.secret_access_key = xml_value(response->body, "SecretAccessKey").value_or("");
    result.session_token = xml_value(response->body, "SessionToken").value_or("");
    result.expires_at = iso8601_epoch(xml_value(response->body, "Expiration").value_or(""));
    if (result.access_key_id.empty() || result.secret_access_key.empty() ||
        result.session_token.empty() || result.expires_at <= std::time(nullptr)) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "AWS web identity response is incomplete"});
    }
    return result;
}

} // namespace

std::expected<ProviderCredentials, RequestError> load_aws_credentials(const ConfigSnapshot& config,
                                                                      std::string_view path) {
    // Keep this order in sync with CONFIGURATION.md: explicit values, profile
    // or web identity, container credentials, then instance metadata.
    ProviderCredentials direct;
    direct.access_key_id = config.option(path, "AWS_ACCESS_KEY_ID");
    direct.secret_access_key = config.option(path, "AWS_SECRET_ACCESS_KEY");
    direct.session_token = config.option(path, "AWS_SESSION_TOKEN");
    if (!direct.access_key_id.empty() || !direct.secret_access_key.empty()) {
        if (direct.access_key_id.empty() || direct.secret_access_key.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                std::string(path) + ": AWS access key and secret must be set together"});
        }
        return direct;
    }

    const std::string profile = config.option(path, "AWS_PROFILE", "default");
    const bool profile_was_explicit = config.has_option(path, "AWS_PROFILE");
    auto loaded_profile = read_aws_profile(config, path, profile, "AWS_CONFIG_FILE",
                                           "AWS_SHARED_CREDENTIALS_FILE", "AWS");
    if (!loaded_profile)
        return std::unexpected(loaded_profile.error());
    if (profile_was_explicit && !loaded_profile->found) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "AWS profile '" + profile + "' was not found"});
    }

    std::string role = config.option(path, "AWS_ROLE_ARN", loaded_profile->value("role_arn"));
    std::string web_token = config.expand_user_path(config.option(
        path, "AWS_WEB_IDENTITY_TOKEN_FILE", loaded_profile->value("web_identity_token_file")));
    if (!role.empty() && !web_token.empty()) {
        auto credentials =
            aws_web_identity(role, web_token,
                             config.option(path, "AWS_ROLE_SESSION_NAME",
                                           loaded_profile->value("role_session_name")),
                             config.option(path, "AWS_STS_ENDPOINT", "https://sts.amazonaws.com/"),
                             config.http_options(path));
        if (credentials)
            credentials->region = loaded_profile->value("region");
        return credentials;
    }
    if (!role.empty()) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         "AWS chained AssumeRole profiles require a custom credential provider; "
                         "credential_process and web identity profiles are supported natively"});
    }
    auto profile_credentials = credentials_from_aws_profile(
        *loaded_profile, "AWS", config.client_options().request_timeout_seconds);
    if (!profile_credentials)
        return std::unexpected(profile_credentials.error());
    if (!profile_credentials->access_key_id.empty())
        return profile_credentials;

    std::string container_url = config.option(path, "AWS_CONTAINER_CREDENTIALS_FULL_URI");
    if (container_url.empty()) {
        const std::string relative = config.option(path, "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
        if (!relative.empty())
            container_url = "http://169.254.170.2" + relative;
    }
    if (!container_url.empty()) {
        if (!safe_container_endpoint(container_url)) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                "AWS_CONTAINER_CREDENTIALS_FULL_URI must use HTTPS or a loopback/ECS host"});
        }
        std::string authorization = config.option(path, "AWS_CONTAINER_AUTHORIZATION_TOKEN");
        const std::string token_file =
            config.expand_user_path(config.option(path, "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE"));
        if (authorization.empty() && !token_file.empty()) {
            auto loaded = read_text_file(token_file, "AWS container authorization token");
            if (!loaded)
                return std::unexpected(loaded.error());
            authorization = trim(*loaded);
        }
        std::vector<Header> headers;
        if (!authorization.empty())
            headers.emplace_back("Authorization", authorization);
        auto response =
            credential_request("GET", container_url, {}, headers, 2, config.http_options(path));
        if (!response)
            return std::unexpected(response.error());
        if (response->status < 200 || response->status >= 300)
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS, "AWS container credentials request failed"});
        ProviderCredentials result = aws_json_credentials(response->body);
        if (result.access_key_id.empty() || result.secret_access_key.empty() ||
            result.session_token.empty() || result.expires_at <= std::time(nullptr))
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS, "AWS container credentials are incomplete"});
        return result;
    }

    long metadata_timeout = 1;
    const std::string metadata_timeout_text =
        config.option(path, "AWS_METADATA_SERVICE_TIMEOUT", "1");
    const auto parsed_timeout = std::from_chars(
        metadata_timeout_text.data(), metadata_timeout_text.data() + metadata_timeout_text.size(),
        metadata_timeout);
    if (parsed_timeout.ec != std::errc{} ||
        parsed_timeout.ptr != metadata_timeout_text.data() + metadata_timeout_text.size() ||
        metadata_timeout < 1 || metadata_timeout > 60) {
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "AWS_METADATA_SERVICE_TIMEOUT must be in [1, 60]"});
    }
    const bool metadata_setting = config.has_option(path, "AWS_EC2_METADATA_DISABLED");
    const bool metadata_disabled =
        option_is_true(config.option(path, "AWS_EC2_METADATA_DISABLED", "NO"));
    const bool try_imds =
        !metadata_disabled && (config.discover_default_credentials() || metadata_setting);
    if (try_imds) {
        auto token = credential_request("PUT", "http://169.254.169.254/latest/api/token", {},
                                        {{"X-aws-ec2-metadata-token-ttl-seconds", "21600"}},
                                        metadata_timeout, config.http_options(path));
        if (token && token->status >= 200 && token->status < 300) {
            const std::vector<Header> headers{{"X-aws-ec2-metadata-token", trim(token->body)}};
            auto role_name = credential_request(
                "GET", "http://169.254.169.254/latest/meta-data/iam/security-credentials/", {},
                headers, metadata_timeout, config.http_options(path));
            if (role_name && role_name->status >= 200 && role_name->status < 300) {
                const std::string role_url =
                    "http://169.254.169.254/latest/meta-data/iam/security-credentials/" +
                    form_encode(trim(role_name->body));
                auto response = credential_request("GET", role_url, {}, headers, metadata_timeout,
                                                   config.http_options(path));
                if (response && response->status >= 200 && response->status < 300) {
                    ProviderCredentials result = aws_json_credentials(response->body);
                    if (!result.access_key_id.empty() && !result.secret_access_key.empty() &&
                        !result.session_token.empty() && result.expires_at > std::time(nullptr))
                        return result;
                }
            }
        }
    }
    return ProviderCredentials{};
}

} // namespace karu::backends
