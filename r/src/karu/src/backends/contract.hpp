#ifndef KARU_BACKENDS_CONTRACT_HPP
#define KARU_BACKENDS_CONTRACT_HPP

#include "../config.hpp"
#include "../locator.hpp"
#include "../request.hpp"

#include <cstdint>
#include <ctime>
#include <expected>
#include <span>
#include <string_view>

namespace karu::backends {

// Everything a cloud backend needs to materialize one ranged GET. Adding a
// provider means implementing this contract in its own translation unit and
// registering it in registry.cpp.
struct RequestContext {
    const ConfigSnapshot& config;
    const Resolved& object;
    const ProviderCredentials* credentials;
    std::string_view range;
    std::string_view region_hint;
    std::string_view if_match;
    std::time_t signing_time;
};

using LoadCredentials = std::expected<ProviderCredentials, RequestError> (*)(const ConfigSnapshot&,
                                                                             std::string_view);
using PrepareRequest = std::expected<PreparedRequest, RequestError> (*)(const RequestContext&);

struct CloudProvider {
    Backend backend;
    karu_credentials_kind credentials_kind;
    std::string_view no_sign_option;
    bool anonymous_by_default;
    std::span<const std::string_view> credential_options;
    LoadCredentials load_credentials;
    PrepareRequest prepare_request;
};

[[nodiscard]] const CloudProvider& s3_provider() noexcept;
[[nodiscard]] const CloudProvider& gcs_provider() noexcept;
[[nodiscard]] const CloudProvider& azure_provider() noexcept;
[[nodiscard]] const CloudProvider& source_provider() noexcept;
[[nodiscard]] const CloudProvider* cloud_provider(Backend backend) noexcept;

[[nodiscard]] std::expected<PreparedRequest, RequestError>
prepare_http(const ConfigSnapshot& config, const Resolved& object, std::string_view if_match);

[[nodiscard]] std::expected<PreparedRequest, RequestError>
prepare_hugging_face(const ConfigSnapshot& config, const Resolved& object,
                     std::string_view if_match);

} // namespace karu::backends

#endif
