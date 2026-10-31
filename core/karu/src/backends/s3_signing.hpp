#ifndef KARU_BACKENDS_S3_SIGNING_HPP
#define KARU_BACKENDS_S3_SIGNING_HPP

#include "../request.hpp"

#include <cstdint>
#include <ctime>
#include <expected>
#include <string>
#include <string_view>

namespace karu::backends {

struct S3GetRequest {
    std::string endpoint;
    std::string_view endpoint_option;
    std::string_view bucket;
    std::string_view key;
    std::string_view region;
    bool virtual_hosting = false;
    std::string_view request_payer;
};

[[nodiscard]] bool valid_aws_region(std::string_view region) noexcept;

// Materialize one GET against an S3-compatible endpoint. A null credential
// pointer deliberately produces an unsigned request.
[[nodiscard]] std::expected<PreparedRequest, RequestError>
prepare_s3_get(const S3GetRequest& target, const ProviderCredentials* credentials,
               std::string_view range, std::string_view if_match, std::time_t signing_time);

} // namespace karu::backends

#endif
