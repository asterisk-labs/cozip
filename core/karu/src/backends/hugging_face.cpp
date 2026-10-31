#include "contract.hpp"
#include "credentials.hpp"
#include "url.hpp"

namespace karu::backends {
namespace {

std::expected<std::string, RequestError> hugging_face_token(const ConfigSnapshot& config,
                                                            std::string_view path) {
    if (std::string token = config.option(path, "HF_TOKEN"); !token.empty())
        return token;

    const std::string token_path = config.hugging_face_token_path(path);
    if (token_path.empty())
        return std::string{};

    auto exists = path_exists(token_path, "Hugging Face token");
    if (!exists)
        return std::unexpected(exists.error());
    if (!*exists)
        return std::string{};

    auto contents = read_text_file(token_path, "Hugging Face token");
    if (!contents)
        return std::unexpected(contents.error());
    return trim(std::move(*contents));
}

} // namespace

std::expected<PreparedRequest, RequestError> prepare_hugging_face(const ConfigSnapshot& config,
                                                                  const Resolved& object,
                                                                  std::string_view if_match) {
    std::string endpoint = without_trailing_slash(
        config.option(object.canonical_uri, "HF_ENDPOINT", "https://huggingface.co"));
    auto normalized_endpoint = http_endpoint(std::move(endpoint), "HF_ENDPOINT");
    if (!normalized_endpoint)
        return std::unexpected(normalized_endpoint.error());
    endpoint = without_trailing_slash(std::move(*normalized_endpoint));
    auto endpoint_parts = split_url(endpoint);
    if (!endpoint_parts)
        return std::unexpected(endpoint_parts.error());
    if (!endpoint_parts->query.empty())
        return std::unexpected(RequestError{KARU_ERR_CONFIG, "HF_ENDPOINT cannot contain a query"});

    PreparedRequest request{endpoint + object.target, {}};
    auto token = hugging_face_token(config, object.canonical_uri);
    if (!token)
        return std::unexpected(token.error());
    if (!token->empty())
        request.headers.emplace_back("Authorization", "Bearer " + *token);
    if (!if_match.empty())
        request.headers.emplace_back("If-Match", if_match);
    if (auto headers = add_configured_headers(config, object.canonical_uri, request.headers, true);
        !headers) {
        return std::unexpected(headers.error());
    }
    return request;
}

} // namespace karu::backends
