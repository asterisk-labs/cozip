#include "contract.hpp"
#include "credentials.hpp"

namespace karu::backends {

std::expected<PreparedRequest, RequestError>
prepare_http(const ConfigSnapshot& config, const Resolved& object, std::string_view if_match) {
    PreparedRequest request{object.target, {}};
    if (!if_match.empty())
        request.headers.emplace_back("If-Match", if_match);
    if (auto headers = add_configured_headers(config, object.canonical_uri, request.headers, false);
        !headers) {
        return std::unexpected(headers.error());
    }
    return request;
}

} // namespace karu::backends
