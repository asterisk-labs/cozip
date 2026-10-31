#ifndef KARU_BACKENDS_CRYPTO_HPP
#define KARU_BACKENDS_CRYPTO_HPP

#include "../request.hpp"

#include <array>
#include <expected>
#include <openssl/evp.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace karu::backends {

using Sha256Digest = std::array<unsigned char, 32>;

[[nodiscard]] std::expected<std::vector<unsigned char>, RequestError>
hmac(const EVP_MD* algorithm, std::span<const unsigned char> key, std::string_view value);
[[nodiscard]] std::expected<Sha256Digest, RequestError> sha256(std::string_view value);
[[nodiscard]] std::expected<Sha256Digest, RequestError>
hmac_sha256(std::span<const unsigned char> key, std::string_view value);
[[nodiscard]] std::string base64(std::span<const unsigned char> bytes);

} // namespace karu::backends

#endif
