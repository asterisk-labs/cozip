#include "source_credentials.hpp"

#include "aws_profile.hpp"

namespace karu::backends {

std::expected<ProviderCredentials, RequestError>
load_source_credentials(const ConfigSnapshot& config, std::string_view path) {
    ProviderCredentials direct;
    direct.access_key_id = config.option(path, "SOURCE_ACCESS_KEY_ID");
    direct.secret_access_key = config.option(path, "SOURCE_SECRET_ACCESS_KEY");
    direct.session_token = config.option(path, "SOURCE_SESSION_TOKEN");
    if (!direct.access_key_id.empty() || !direct.secret_access_key.empty()) {
        if (direct.access_key_id.empty() || direct.secret_access_key.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                std::string(path) + ": Source access key and secret must be set together"});
        }
        return direct;
    }

    const std::string profile_name = config.option(path, "SOURCE_PROFILE", "source-coop");
    auto profile = read_aws_profile(config, path, profile_name, "SOURCE_CONFIG_FILE",
                                    "SOURCE_SHARED_CREDENTIALS_FILE", "Source");
    if (!profile)
        return std::unexpected(profile.error());
    if (!profile->found) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, std::string(path) + ": Source profile '" +
                                                   profile_name + "' was not found"});
    }

    auto credentials = credentials_from_aws_profile(
        *profile, "Source", config.client_options().request_timeout_seconds);
    if (!credentials)
        return std::unexpected(credentials.error());
    if (credentials->access_key_id.empty() || credentials->secret_access_key.empty()) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, std::string(path) + ": Source profile '" +
                                                   profile_name + "' did not provide credentials"});
    }
    return credentials;
}

} // namespace karu::backends
