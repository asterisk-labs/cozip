#ifndef KARU_REQUEST_BUILDER_HPP
#define KARU_REQUEST_BUILDER_HPP

#include "config.hpp"
#include "credential_cache.hpp"
#include "locator.hpp"
#include "request.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace karu {

namespace backends {
struct CloudProvider;
}

// Per-client request materializer. Locators stay free of endpoints and
// credentials; this object resolves and signs after the byte range is known.
class RequestBuilder {
  public:
    explicit RequestBuilder(ConfigSnapshot config) : config_(std::move(config)) {}

    [[nodiscard]] std::expected<PreparedRequest, RequestError>
    prepare(const Locator& locator, std::uint64_t first, std::uint64_t length,
            std::string_view region_hint = {}, std::string_view if_match = {});

    [[nodiscard]] std::expected<ResolvedCredentials, RequestError>
    resolve_credentials(const Locator& locator);

    [[nodiscard]] std::expected<PreparedRequest, RequestError>
    materialize(const Locator& locator, const ResolvedCredentials& credentials, std::uint64_t first,
                std::uint64_t length, std::string_view region_hint = {},
                std::string_view if_match = {});

    // The last `length` bytes of the object, whatever its size.
    [[nodiscard]] std::expected<PreparedRequest, RequestError>
    prepare_suffix(const Locator& locator, std::uint64_t length, std::string_view region_hint = {},
                   std::string_view if_match = {});

    void invalidate_credentials(const Locator& locator);

    [[nodiscard]] const ConfigSnapshot& config() const noexcept { return config_; }

  private:
    [[nodiscard]] std::expected<PreparedRequest, RequestError>
    materialize_range(const Locator& locator, const ResolvedCredentials& credentials,
                      std::string range, std::string_view region_hint, std::string_view if_match);

    ConfigSnapshot config_;
    CredentialCache credentials_;
};

} // namespace karu

#endif
