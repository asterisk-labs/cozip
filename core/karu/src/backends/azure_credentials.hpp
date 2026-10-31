#ifndef KARU_BACKENDS_AZURE_CREDENTIALS_HPP
#define KARU_BACKENDS_AZURE_CREDENTIALS_HPP

#include "credentials.hpp"

namespace karu::backends {

[[nodiscard]] std::expected<ProviderCredentials, RequestError>
load_azure_credentials(const ConfigSnapshot& config, std::string_view path);

} // namespace karu::backends

#endif
