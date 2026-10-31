#include "aws_profile.hpp"

#include "../process.hpp"

#include <chrono>

namespace karu::backends {
namespace {

std::expected<std::string, RequestError>
run_credential_process(const std::string& command, std::string_view label, long timeout_seconds) {
    constexpr std::size_t output_limit = 1u << 20;
    const auto timeout = std::chrono::seconds(timeout_seconds);
    auto command_result = os::run_command(command, output_limit, timeout);
    if (!command_result) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, std::string(label) + " credential_process could not start: " +
                                      command_result.error().message()});
    }
    if (command_result->timed_out) {
        return std::unexpected(RequestError{
            KARU_TIMEOUT, std::string(label) + " credential_process exceeded the request timeout"});
    }
    if (command_result->output_limit_exceeded) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         std::string(label) + " credential_process returned more than 1 MiB"});
    }
    if (command_result->exit_code != 0) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, std::string(label) + " credential_process exited with status " +
                                      std::to_string(command_result->exit_code)});
    }
    return std::move(command_result->output);
}

} // namespace

std::string AwsProfile::value(std::string_view name) const {
    const auto entry = values.find(std::string(name));
    return entry == values.end() ? std::string{} : entry->second;
}

std::expected<AwsProfile, RequestError>
read_aws_profile(const ConfigSnapshot& config, std::string_view path, std::string_view profile,
                 std::string_view config_file_option, std::string_view credentials_file_option,
                 std::string_view label) {
    const std::string config_path = config.expand_user_path(
        config.option(path, config_file_option, config.default_aws_path("config")));
    const std::string credentials_path = config.expand_user_path(
        config.option(path, credentials_file_option, config.default_aws_path("credentials")));

    auto config_exists = path_exists(config_path, std::string(label) + " config file");
    if (!config_exists)
        return std::unexpected(config_exists.error());
    auto credentials_exists =
        path_exists(credentials_path, std::string(label) + " shared credentials file");
    if (!credentials_exists)
        return std::unexpected(credentials_exists.error());

    AwsProfile result;
    if (config.has_option(path, config_file_option) && !*config_exists) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS,
                                            std::string(label) + " config file does not exist: '" +
                                                config_path + "'"});
    }
    if (*config_exists) {
        auto file = read_ini(config_path, std::string(label) + " config");
        if (!file)
            return std::unexpected(file.error());
        const std::string section =
            profile == "default" ? "default" : "profile " + std::string(profile);
        if (const auto found = file->find(section); found != file->end()) {
            result.values = found->second;
            result.found = true;
        }
    }

    if (config.has_option(path, credentials_file_option) && !*credentials_exists) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, std::string(label) +
                                                   " shared credentials file does not exist: '" +
                                                   credentials_path + "'"});
    }
    if (*credentials_exists) {
        auto file = read_ini(credentials_path, std::string(label) + " shared credentials");
        if (!file)
            return std::unexpected(file.error());
        if (const auto found = file->find(std::string(profile)); found != file->end()) {
            for (const auto& [name, value] : found->second)
                result.values.insert_or_assign(name, value);
            result.found = true;
        }
    }
    return result;
}

ProviderCredentials aws_json_credentials(std::string_view json) {
    ProviderCredentials result;
    result.access_key_id = json_string(json, "AccessKeyId").value_or("");
    result.secret_access_key = json_string(json, "SecretAccessKey").value_or("");
    result.session_token =
        json_string(json, "Token").value_or(json_string(json, "SessionToken").value_or(""));
    if (auto expiration = json_string(json, "Expiration"))
        result.expires_at = iso8601_epoch(*expiration);
    return result;
}

std::expected<ProviderCredentials, RequestError>
credentials_from_aws_profile(const AwsProfile& profile, std::string_view label,
                             long timeout_seconds) {
    if (!profile.value("sso_session").empty() || !profile.value("sso_start_url").empty()) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         std::string(label) +
                             " IAM Identity Center profiles require a custom credential provider"});
    }
    if (!profile.value("role_arn").empty() || !profile.value("source_profile").empty() ||
        !profile.value("web_identity_token_file").empty()) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS,
            std::string(label) + " AssumeRole profiles require a custom credential provider"});
    }
    if (const std::string command = profile.value("credential_process"); !command.empty()) {
        auto output = run_credential_process(command, label, timeout_seconds);
        if (!output)
            return std::unexpected(output.error());
        ProviderCredentials result = aws_json_credentials(*output);
        if (result.access_key_id.empty() || result.secret_access_key.empty()) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS,
                             std::string(label) + " credential_process response is incomplete"});
        }
        result.region = profile.value("region");
        return result;
    }

    ProviderCredentials result;
    result.access_key_id = profile.value("aws_access_key_id");
    result.secret_access_key = profile.value("aws_secret_access_key");
    result.session_token = profile.value("aws_session_token");
    result.region = profile.value("region");
    if (!result.access_key_id.empty() || !result.secret_access_key.empty()) {
        if (result.access_key_id.empty() || result.secret_access_key.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                std::string(label) + " profile access key and secret must be set together"});
        }
    }
    return result;
}

} // namespace karu::backends
