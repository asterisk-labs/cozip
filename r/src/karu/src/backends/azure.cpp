#include "azure_credentials.hpp"
#include "contract.hpp"
#include "crypto.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <span>
#include <unordered_map>

namespace karu::backends {
namespace {

// Sent on every request. Without it Azure serves anonymous requests as 2009-09-19,
// whose unquoted ETags cannot pin reads, and SAS requests as the token's sv. The sv
// still decides how Azure checks the SAS itself.
constexpr std::string_view SERVICE_VERSION = "2023-11-03";

constexpr std::array<std::string_view, 18> CREDENTIAL_OPTIONS{"AZURE_STORAGE_CONNECTION_STRING",
                                                              "AZURE_STORAGE_ACCOUNT",
                                                              "AZURE_STORAGE_ACCESS_KEY",
                                                              "AZURE_STORAGE_SAS_TOKEN",
                                                              "AZURE_STORAGE_ACCESS_TOKEN",
                                                              "AZURE_TENANT_ID",
                                                              "AZURE_CLIENT_ID",
                                                              "AZURE_CLIENT_SECRET",
                                                              "AZURE_FEDERATED_TOKEN_FILE",
                                                              "AZURE_AUTHORITY_HOST",
                                                              "AZURE_STORAGE_SCOPE",
                                                              "AZURE_STORAGE_RESOURCE",
                                                              "AZURE_IMDS_OBJECT_ID",
                                                              "AZURE_IMDS_CLIENT_ID",
                                                              "AZURE_IMDS_MSI_RES_ID",
                                                              "IDENTITY_ENDPOINT",
                                                              "IDENTITY_HEADER",
                                                              "IMDS_ENDPOINT"};

std::unordered_map<std::string, std::string> parse_connection_string(std::string_view connection) {
    std::unordered_map<std::string, std::string> result;
    std::size_t start = 0;
    while (start <= connection.size()) {
        const std::size_t end = connection.find(';', start);
        const std::string_view item = connection.substr(start, end - start);
        const std::size_t equals = item.find('=');
        if (equals != std::string_view::npos)
            result.emplace(lower(std::string(item.substr(0, equals))),
                           std::string(item.substr(equals + 1)));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::expected<std::vector<unsigned char>, RequestError> decode_base64(std::string value,
                                                                      std::string_view context) {
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c) != 0; }),
                value.end());
    if (value.empty() || value.size() % 4 != 0)
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, std::string(context) + " is not base64"});
    std::vector<unsigned char> result(value.size() / 4 * 3);
    const int decoded =
        EVP_DecodeBlock(result.data(), reinterpret_cast<const unsigned char*>(value.data()),
                        static_cast<int>(value.size()));
    if (decoded < 0)
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, std::string(context) + " is not base64"});
    std::size_t length = static_cast<std::size_t>(decoded);
    if (value.ends_with("=="))
        length -= 2;
    else if (value.ends_with('='))
        --length;
    result.resize(length);
    return result;
}

std::expected<PreparedRequest, RequestError>
prepare_azure(const ConfigSnapshot& config, const Resolved& object,
              const ProviderCredentials* custom, std::string_view range, std::string_view if_match,
              std::time_t signing_time) {
    const std::string& path = object.canonical_uri;
    const auto connection =
        parse_connection_string(config.option(path, "AZURE_STORAGE_CONNECTION_STRING"));
    auto connection_value = [&](std::string_view name) {
        const auto iterator = connection.find(std::string(name));
        return iterator == connection.end() ? std::string{} : iterator->second;
    };

    ProviderCredentials credentials;
    if (custom != nullptr)
        credentials = *custom;
    credentials.account_name =
        credentials.account_name.empty()
            ? config.option(path, "AZURE_STORAGE_ACCOUNT", connection_value("accountname"))
            : credentials.account_name;
    credentials.secret_access_key =
        credentials.secret_access_key.empty()
            ? config.option(path, "AZURE_STORAGE_ACCESS_KEY", connection_value("accountkey"))
            : credentials.secret_access_key;
    credentials.sas_token = credentials.sas_token.empty()
                                ? config.option(path, "AZURE_STORAGE_SAS_TOKEN",
                                                connection_value("sharedaccesssignature"))
                                : credentials.sas_token;
    credentials.bearer_token = credentials.bearer_token.empty()
                                   ? config.option(path, "AZURE_STORAGE_ACCESS_TOKEN")
                                   : credentials.bearer_token;

    std::string endpoint = config.option(path, "AZURE_STORAGE_ENDPOINT");
    if (endpoint.empty())
        endpoint = connection_value(object.azure_dfs ? "dfsendpoint" : "blobendpoint");
    if (endpoint.empty()) {
        if (credentials.account_name.empty()) {
            return std::unexpected(
                RequestError{KARU_ERR_CONFIG,
                             path + ": Azure needs AZURE_STORAGE_ACCOUNT or an explicit endpoint"});
        }
        std::string suffix = connection_value("endpointsuffix");
        if (suffix.empty())
            suffix = "core.windows.net";
        std::string protocol = lower(connection_value("defaultendpointsprotocol"));
        if (protocol.empty())
            protocol = "https";
        if (protocol != "http" && protocol != "https") {
            return std::unexpected(RequestError{
                KARU_ERR_CONFIG, "Azure DefaultEndpointsProtocol must be http or https"});
        }
        endpoint = protocol + "://" + credentials.account_name +
                   (object.azure_dfs ? ".dfs." : ".blob.") + suffix;
    } else {
        auto normalized_endpoint = http_endpoint(std::move(endpoint), "AZURE_STORAGE_ENDPOINT");
        if (!normalized_endpoint)
            return std::unexpected(normalized_endpoint.error());
        endpoint = std::move(*normalized_endpoint);
    }
    auto endpoint_parts = split_url(endpoint);
    if (!endpoint_parts)
        return std::unexpected(endpoint_parts.error());
    if (!endpoint_parts->query.empty())
        return std::unexpected(
            RequestError{KARU_ERR_CONFIG, "AZURE_STORAGE_ENDPOINT cannot contain a query"});
    std::string url = append_object(endpoint, object.container, object.key);

    std::vector<Header> headers;
    if (!if_match.empty())
        headers.emplace_back("If-Match", if_match);
    headers.emplace_back("x-ms-version", SERVICE_VERSION);
    if (option_is_true(config.option(path, "AZURE_NO_SIGN_REQUEST", "NO")))
        return PreparedRequest{std::move(url), std::move(headers)};
    if (!credentials.sas_token.empty()) {
        std::string sas = credentials.sas_token;
        while (!sas.empty() && (sas.front() == '?' || sas.front() == '&'))
            sas.erase(sas.begin());
        url += (url.find('?') == std::string::npos ? "?" : "&") + sas;
        PreparedRequest request{std::move(url), std::move(headers)};
        request.http.follow_redirects = false;
        return request;
    }
    if (!credentials.bearer_token.empty()) {
        headers.emplace_back("Authorization", "Bearer " + credentials.bearer_token);
        PreparedRequest request{std::move(url), std::move(headers)};
        request.http.follow_redirects = false;
        return request;
    }
    if (credentials.account_name.empty() || credentials.secret_access_key.empty()) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, path + ": no Azure credentials; configure a SAS, access token or "
                                         "account key, install a custom provider, or set "
                                         "AZURE_NO_SIGN_REQUEST=YES"});
    }

    auto decoded_key = decode_base64(credentials.secret_access_key, "AZURE_STORAGE_ACCESS_KEY");
    if (!decoded_key)
        return std::unexpected(decoded_key.error());
    const std::string date = rfc7231_date(signing_time);
    const std::string canonical_headers =
        "x-ms-date:" + date + "\nx-ms-version:" + std::string(SERVICE_VERSION) + "\n";
    auto url_parts = split_url(url);
    if (!url_parts)
        return std::unexpected(url_parts.error());
    const std::string canonical_resource = "/" + credentials.account_name + url_parts->path;
    const std::array<std::string_view, 11> standard_headers{"", "",       "", "", "",   "",
                                                            "", if_match, "", "", range};
    std::string string_to_sign = "GET\n";
    for (std::string_view value : standard_headers) {
        string_to_sign.append(value);
        string_to_sign.push_back('\n');
    }
    string_to_sign += canonical_headers + canonical_resource;
    auto signature = hmac_sha256(*decoded_key, string_to_sign);
    if (!signature)
        return std::unexpected(signature.error());
    headers.emplace_back("x-ms-date", date);
    headers.emplace_back("Authorization",
                         "SharedKey " + credentials.account_name + ":" + base64(*signature));
    PreparedRequest request{std::move(url), std::move(headers)};
    request.http.follow_redirects = false;
    return request;
}

} // namespace

const CloudProvider& azure_provider() noexcept {
    static const CloudProvider provider{
        Backend::Azure,
        KARU_CREDENTIALS_AZURE,
        "AZURE_NO_SIGN_REQUEST",
        false,
        CREDENTIAL_OPTIONS,
        load_azure_credentials,
        [](const RequestContext& request) {
            return prepare_azure(request.config, request.object, request.credentials, request.range,
                                 request.if_match, request.signing_time);
        }};
    return provider;
}

} // namespace karu::backends
