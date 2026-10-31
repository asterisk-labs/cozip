#ifndef KARU_BACKENDS_S3_CREDENTIALS_HPP
#define KARU_BACKENDS_S3_CREDENTIALS_HPP

#include "credentials.hpp"

namespace karu::backends {

std::expected<ProviderCredentials, RequestError> load_aws_credentials(const ConfigSnapshot& config,
                                                                      std::string_view path);

} // namespace karu::backends

#endif
