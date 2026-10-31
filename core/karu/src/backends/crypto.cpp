#include "crypto.hpp"

#include <cstring>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

namespace karu::backends {
namespace {

constexpr std::size_t kSha256Block = 64;

// Fetch SHA-256 once instead of resolving the algorithm for each request.
const EVP_MD* sha256_method() noexcept {
    static EVP_MD* const method = EVP_MD_fetch(nullptr, "SHA256", nullptr);
    return method;
}

// Reuse the digest context within each signing thread.
EVP_MD_CTX* thread_digest_context() noexcept {
    struct Context {
        EVP_MD_CTX* value = EVP_MD_CTX_new();
        Context() = default;
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        ~Context() { EVP_MD_CTX_free(value); }
    };
    thread_local Context context;
    return context.value;
}

bool digest_parts(std::span<const std::span<const unsigned char>> parts,
                  Sha256Digest& out) noexcept {
    const EVP_MD* method = sha256_method();
    EVP_MD_CTX* context = thread_digest_context();
    if (method == nullptr || context == nullptr ||
        EVP_DigestInit_ex2(context, method, nullptr) != 1) {
        return false;
    }
    for (const auto part : parts) {
        if (EVP_DigestUpdate(context, part.data(), part.size()) != 1)
            return false;
    }
    unsigned int length = 0;
    return EVP_DigestFinal_ex(context, out.data(), &length) == 1 && length == out.size();
}

std::span<const unsigned char> bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<const unsigned char*>(text.data()), text.size()};
}

} // namespace

std::expected<std::vector<unsigned char>, RequestError>
hmac(const EVP_MD* algorithm, std::span<const unsigned char> key, std::string_view value) {
    if (algorithm == nullptr)
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "HMAC algorithm is unavailable"});
    const int digest_size = EVP_MD_get_size(algorithm);
    if (digest_size <= 0 ||
        key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "HMAC parameters are not supported"});
    }
    std::vector<unsigned char> result(static_cast<std::size_t>(digest_size));
    unsigned int length = 0;
    if (HMAC(algorithm, key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(value.data()), value.size(), result.data(),
             &length) == nullptr) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "OpenSSL HMAC failed"});
    }
    result.resize(length);
    return result;
}

std::expected<Sha256Digest, RequestError> sha256(std::string_view value) {
    Sha256Digest digest{};
    const std::array parts{bytes_of(value)};
    if (!digest_parts(parts, digest))
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "OpenSSL SHA256 failed"});
    return digest;
}

std::expected<Sha256Digest, RequestError> hmac_sha256(std::span<const unsigned char> key,
                                                      std::string_view value) {
    // RFC 2104: H((K ^ opad) || H((K ^ ipad) || message)), where a key longer
    // than one block is replaced by its digest.
    std::array<unsigned char, kSha256Block> block{};
    bool ok = true;
    if (key.size() > block.size()) {
        Sha256Digest hashed{};
        const std::array parts{key};
        ok = digest_parts(parts, hashed);
        std::memcpy(block.data(), hashed.data(), hashed.size());
        OPENSSL_cleanse(hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(block.data(), key.data(), key.size());
    }

    std::array<unsigned char, kSha256Block> pad{};
    Sha256Digest inner{};
    Sha256Digest result{};
    if (ok) {
        for (std::size_t index = 0; index < block.size(); ++index)
            pad[index] = static_cast<unsigned char>(block[index] ^ 0x36);
        const std::array inner_parts{std::span<const unsigned char>(pad), bytes_of(value)};
        ok = digest_parts(inner_parts, inner);
    }
    if (ok) {
        for (std::size_t index = 0; index < block.size(); ++index)
            pad[index] = static_cast<unsigned char>(block[index] ^ 0x5c);
        const std::array outer_parts{std::span<const unsigned char>(pad),
                                     std::span<const unsigned char>(inner)};
        ok = digest_parts(outer_parts, result);
    }
    OPENSSL_cleanse(block.data(), block.size());
    OPENSSL_cleanse(pad.data(), pad.size());
    OPENSSL_cleanse(inner.data(), inner.size());
    if (!ok)
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, "OpenSSL HMAC failed"});
    return result;
}

std::string base64(std::span<const unsigned char> bytes) {
    if (bytes.empty())
        return {};
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                        bytes.data(), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(written));
    return result;
}

} // namespace karu::backends
