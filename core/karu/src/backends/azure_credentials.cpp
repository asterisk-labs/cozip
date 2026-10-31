#include "azure_credentials.hpp"

#include "../text.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <span>
#include <unordered_map>

namespace karu::backends {

std::expected<ProviderCredentials, RequestError>
load_azure_credentials(const ConfigSnapshot& config, std::string_view path) {
    // Prefer storage credentials, then service-principal or workload identity,
    // App Service, and finally managed identity from IMDS.
    ProviderCredentials direct;
    direct.account_name = config.option(path, "AZURE_STORAGE_ACCOUNT");
    direct.secret_access_key = config.option(path, "AZURE_STORAGE_ACCESS_KEY");
    direct.sas_token = config.option(path, "AZURE_STORAGE_SAS_TOKEN");
    direct.bearer_token = config.option(path, "AZURE_STORAGE_ACCESS_TOKEN");
    if (config.has_option(path, "AZURE_STORAGE_CONNECTION_STRING"))
        return direct;
    if (!direct.sas_token.empty() || !direct.bearer_token.empty() ||
        !direct.secret_access_key.empty())
        return direct;

    const std::string tenant = config.option(path, "AZURE_TENANT_ID");
    const std::string client = config.option(path, "AZURE_CLIENT_ID");
    const std::string secret = config.option(path, "AZURE_CLIENT_SECRET");
    const std::string federated_file =
        config.expand_user_path(config.option(path, "AZURE_FEDERATED_TOKEN_FILE"));
    if (!tenant.empty() && !client.empty() && (!secret.empty() || !federated_file.empty())) {
        std::string assertion;
        std::string grant;
        if (!federated_file.empty()) {
            auto loaded = read_text_file(federated_file, "Azure federated token");
            if (!loaded)
                return std::unexpected(loaded.error());
            assertion = trim(*loaded);
            grant = "&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%"
                    "3Ajwt-bearer" +
                    std::string("&client_assertion=") + form_encode(assertion);
        } else {
            grant = "&client_secret=" + form_encode(secret);
        }
        const std::string authority = without_trailing_slash(
            config.option(path, "AZURE_AUTHORITY_HOST", "https://login.microsoftonline.com"));
        const std::string scope =
            config.option(path, "AZURE_STORAGE_SCOPE", "https://storage.azure.com/.default");
        return oauth_token(authority + "/" + form_encode(tenant) + "/oauth2/v2.0/token",
                           "client_id=" + form_encode(client) + grant +
                               "&scope=" + form_encode(scope) + "&grant_type=client_credentials",
                           {}, config.http_options(path));
    }

    const std::string storage_resource =
        config.option(path, "AZURE_STORAGE_RESOURCE", "https://storage.azure.com/");
    const std::string imds_object = config.option(path, "AZURE_IMDS_OBJECT_ID");
    const std::string imds_client = config.option(path, "AZURE_IMDS_CLIENT_ID", client);
    const std::string imds_resource = config.option(path, "AZURE_IMDS_MSI_RES_ID");
    const int selector_count = static_cast<int>(!imds_object.empty()) +
                               static_cast<int>(!imds_client.empty()) +
                               static_cast<int>(!imds_resource.empty());
    if (selector_count > 1) {
        return std::unexpected(RequestError{
            KARU_ERR_CONFIG,
            "set only one Azure managed-identity selector: object ID, client ID, or resource ID"});
    }
    const auto add_identity_selector = [&](std::string& url) {
        if (!imds_object.empty())
            url += "&object_id=" + form_encode(imds_object);
        else if (!imds_client.empty())
            url += "&client_id=" + form_encode(imds_client);
        else if (!imds_resource.empty())
            url += "&msi_res_id=" + form_encode(imds_resource);
    };
    const std::string identity_endpoint = config.option(path, "IDENTITY_ENDPOINT");
    if (!identity_endpoint.empty()) {
        std::string url = identity_endpoint;
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += "api-version=2019-08-01&resource=" + form_encode(storage_resource);
        add_identity_selector(url);
        std::vector<Header> headers;
        const std::string identity_header = config.option(path, "IDENTITY_HEADER");
        if (!identity_header.empty())
            headers.emplace_back("X-IDENTITY-HEADER", identity_header);
        auto response = credential_request("GET", url, {}, headers, 2, config.http_options(path));
        if (!response)
            return std::unexpected(response.error());
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                concat("Azure App Service identity returned HTTP ", response->status)});
        }
        ProviderCredentials result;
        result.bearer_token = json_string(response->body, "access_token").value_or("");
        result.expires_at = json_integer(response->body, "expires_on")
                                .value_or(static_cast<std::int64_t>(std::time(nullptr)) + 3600);
        if (result.bearer_token.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS, "Azure App Service identity response has no access token"});
        }
        return result;
    }

    if (!config.discover_default_credentials() && !config.has_option(path, "IMDS_ENDPOINT"))
        return direct;
    std::string imds = config.option(path, "IMDS_ENDPOINT",
                                     "http://169.254.169.254/metadata/identity/oauth2/token");
    imds += (imds.find('?') == std::string::npos ? "?" : "&");
    imds += "api-version=2018-02-01&resource=" + form_encode(storage_resource);
    add_identity_selector(imds);
    auto response =
        credential_request("GET", imds, {}, {{"Metadata", "true"}}, 1, config.http_options(path));
    if (!response && config.has_option(path, "IMDS_ENDPOINT"))
        return std::unexpected(response.error());
    if (response && (response->status < 200 || response->status >= 300) &&
        config.has_option(path, "IMDS_ENDPOINT")) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS,
            concat("Azure managed identity endpoint returned HTTP ", response->status)});
    }
    if (response && response->status >= 200 && response->status < 300) {
        ProviderCredentials result;
        result.bearer_token = json_string(response->body, "access_token").value_or("");
        result.expires_at = json_integer(response->body, "expires_on")
                                .value_or(static_cast<std::int64_t>(std::time(nullptr)) + 3600);
        if (!result.bearer_token.empty())
            return result;
        if (config.has_option(path, "IMDS_ENDPOINT")) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS,
                             "Azure managed identity endpoint response has no access token"});
        }
    }
    return direct;
}

} // namespace karu::backends
