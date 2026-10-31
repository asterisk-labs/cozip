#ifndef KARU_BACKENDS_AWS_PROFILE_HPP
#define KARU_BACKENDS_AWS_PROFILE_HPP

#include "credentials.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace karu::backends {

struct AwsProfile {
    IniSection values;
    bool found = false;

    [[nodiscard]] std::string value(std::string_view name) const;
};

// Reads one AWS-format profile. Source Cooperative intentionally uses this
// format through `credential_process`, but selects its own option namespace.
[[nodiscard]] std::expected<AwsProfile, RequestError>
read_aws_profile(const ConfigSnapshot& config, std::string_view path, std::string_view profile,
                 std::string_view config_file_option, std::string_view credentials_file_option,
                 std::string_view label);

[[nodiscard]] std::expected<ProviderCredentials, RequestError>
credentials_from_aws_profile(const AwsProfile& profile, std::string_view label,
                             long timeout_seconds);

[[nodiscard]] ProviderCredentials aws_json_credentials(std::string_view json);

} // namespace karu::backends

#endif
