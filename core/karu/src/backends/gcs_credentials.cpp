#include "gcs_credentials.hpp"

#include "../text.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "url.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <ranges>
#include <span>

namespace karu::backends {
namespace {

std::string normalize_pem(std::string value) {
    std::size_t position = 0;
    while ((position = value.find("\\n", position)) != std::string::npos) {
        value.replace(position, 2, "\n");
        ++position;
    }
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

std::string base64url(std::span<const unsigned char> bytes) {
    std::string result = base64(bytes);
    std::ranges::replace(result, '+', '-');
    std::ranges::replace(result, '/', '_');
    while (result.ends_with('='))
        result.pop_back();
    return result;
}

std::string base64url(std::string_view text) {
    return base64url(std::span(reinterpret_cast<const unsigned char*>(text.data()), text.size()));
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

std::expected<std::string, RequestError> rsa_sha256(std::string_view private_key,
                                                    std::string_view message) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(private_key.data(), static_cast<int>(private_key.size())), BIO_free);
    if (!bio)
        return std::unexpected(RequestError{KARU_ERR_NOMEM, "cannot allocate key reader"});
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key)
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "service account private_key is invalid"});
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context)
        return std::unexpected(RequestError{KARU_ERR_NOMEM, "cannot allocate signer"});
    if (EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestSignUpdate(context.get(), message.data(), message.size()) != 1) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "cannot initialize service account signature"});
    }
    std::size_t size = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &size) != 1) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "cannot size service account signature"});
    }
    std::vector<unsigned char> signature(size);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &size) != 1) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "cannot sign service account assertion"});
    }
    signature.resize(size);
    return base64url(signature);
}

std::expected<ProviderCredentials, RequestError>
gcs_service_account_values(std::string_view email, std::string_view private_key,
                           std::string_view token_uri, std::string_view scope,
                           const HttpRequestOptions& options) {
    if (email.empty() || private_key.empty())
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "GCS service_account JSON is incomplete"});
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string header = base64url(R"({"alg":"RS256","typ":"JWT"})");
    const std::string claims = "{\"iss\":\"" + json_escape(email) + "\",\"scope\":\"" +
                               json_escape(scope) + "\",\"aud\":\"" + json_escape(token_uri) +
                               "\",\"iat\":" + std::to_string(now) +
                               ",\"exp\":" + std::to_string(now + 3600) + "}";
    const std::string unsigned_assertion = header + "." + base64url(claims);
    auto signature = rsa_sha256(private_key, unsigned_assertion);
    if (!signature)
        return std::unexpected(signature.error());
    return oauth_token(
        std::string(token_uri),
        "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" +
            form_encode(unsigned_assertion + "." + *signature),
        {}, options);
}

std::expected<ProviderCredentials, RequestError>
gcs_service_account(std::string_view json, const ConfigSnapshot& config, std::string_view path) {
    const auto email = json_string(json, "client_email");
    const auto private_key = json_string(json, "private_key");
    if (!email || !private_key)
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "GCS service_account JSON is incomplete"});
    const std::string token_uri =
        json_string(json, "token_uri").value_or("https://oauth2.googleapis.com/token");
    const std::string scope =
        config.option(path, "GCS_SCOPE", "https://www.googleapis.com/auth/devstorage.read_only");
    return gcs_service_account_values(*email, normalize_pem(*private_key), token_uri, scope,
                                      config.http_options(path));
}

std::expected<ProviderCredentials, RequestError>
gcs_authorized_user(std::string_view json, const ConfigSnapshot& config, std::string_view path) {
    const std::string client_id =
        config.option(path, "GCS_CLIENT_ID", json_string(json, "client_id").value_or(""));
    const std::string client_secret =
        config.option(path, "GCS_CLIENT_SECRET", json_string(json, "client_secret").value_or(""));
    const std::string refresh_token =
        config.option(path, "GCS_REFRESH_TOKEN", json_string(json, "refresh_token").value_or(""));
    const std::string token_uri =
        json_string(json, "token_uri").value_or("https://oauth2.googleapis.com/token");
    if (client_id.empty() || client_secret.empty() || refresh_token.empty()) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "GCS authorized_user credentials are incomplete"});
    }
    return oauth_token(
        token_uri,
        "client_id=" + form_encode(client_id) + "&client_secret=" + form_encode(client_secret) +
            "&refresh_token=" + form_encode(refresh_token) + "&grant_type=refresh_token",
        {}, config.http_options(path));
}

std::expected<ProviderCredentials, RequestError>
gcs_external_account(std::string_view json, const ConfigSnapshot& config, std::string_view path) {
    const HttpRequestOptions http = config.http_options(path);
    const auto audience = json_string(json, "audience");
    const auto subject_type = json_string(json, "subject_token_type");
    const std::string token_url =
        json_string(json, "token_url").value_or("https://sts.googleapis.com/v1/token");
    const auto subject_file = json_string(json, "file");
    const auto subject_url = json_string(json, "url");
    const std::string scope =
        config.option(path, "GCS_SCOPE", "https://www.googleapis.com/auth/devstorage.read_only");
    if (!audience || !subject_type || (!subject_file && !subject_url)) {
        return std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, "GCS external_account credential_source is incomplete"});
    }
    std::string subject;
    if (subject_file) {
        auto loaded = read_text_file(config.expand_user_path(*subject_file),
                                     "GCS external account subject token");
        if (!loaded)
            return std::unexpected(loaded.error());
        subject = trim(*loaded);
    } else {
        auto loaded = credential_request("GET", *subject_url, {}, {}, 5, http);
        if (!loaded)
            return std::unexpected(loaded.error());
        if (loaded->status < 200 || loaded->status >= 300) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS, "GCS external account subject endpoint failed"});
        }
        subject = trim(loaded->body);
    }
    if (const auto field = json_string(json, "subject_token_field_name");
        field && !field->empty()) {
        auto extracted = json_string(subject, *field);
        if (!extracted || extracted->empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                "GCS external account response has no configured subject token field"});
        }
        subject = std::move(*extracted);
    }
    auto exchanged = oauth_token(
        token_url,
        "audience=" + form_encode(*audience) +
            "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange" +
            "&requested_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Aaccess_token" +
            "&scope=" + form_encode(scope) + "&subject_token_type=" + form_encode(*subject_type) +
            "&subject_token=" + form_encode(subject),
        {}, http);
    if (!exchanged)
        return std::unexpected(exchanged.error());

    const auto impersonation = json_string(json, "service_account_impersonation_url");
    if (!impersonation || impersonation->empty())
        return exchanged;
    auto response =
        credential_request("POST", *impersonation,
                           "{\"scope\":[\"" + json_escape(scope) + "\"],\"lifetime\":\"3600s\"}",
                           {{"Authorization", "Bearer " + exchanged->bearer_token},
                            {"Content-Type", "application/json"}},
                           5, http);
    if (!response)
        return std::unexpected(response.error());
    if (response->status < 200 || response->status >= 300)
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "GCS service account impersonation failed"});
    ProviderCredentials result;
    result.bearer_token = json_string(response->body, "accessToken").value_or("");
    result.expires_at = iso8601_epoch(json_string(response->body, "expireTime").value_or(""));
    if (result.bearer_token.empty() || result.expires_at <= std::time(nullptr))
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "GCS impersonation response has no token"});
    return result;
}

} // namespace

std::expected<ProviderCredentials, RequestError> load_gcs_credentials(const ConfigSnapshot& config,
                                                                      std::string_view path) {
    // Explicit tokens and keys win, followed by ADC, service-account values,
    // and finally the metadata service.
    ProviderCredentials direct;
    direct.bearer_token = config.option(path, "GCS_ACCESS_TOKEN");
    direct.access_key_id = config.option(path, "GCS_HMAC_ACCESS_KEY_ID");
    direct.secret_access_key = config.option(path, "GCS_HMAC_SECRET_ACCESS_KEY");
    if (!direct.bearer_token.empty())
        return direct;
    if (!direct.access_key_id.empty() || !direct.secret_access_key.empty()) {
        if (direct.access_key_id.empty() || direct.secret_access_key.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS,
                std::string(path) + ": GCS HMAC access key and secret must be set together"});
        }
        return direct;
    }

    if (!config.option(path, "GCS_REFRESH_TOKEN").empty())
        return gcs_authorized_user("{}", config, path);

    std::string adc_path =
        config.expand_user_path(config.option(path, "GOOGLE_APPLICATION_CREDENTIALS"));
    if (adc_path.empty()) {
        const std::string cloud_config =
            config.expand_user_path(config.option(path, "CLOUDSDK_CONFIG"));
        if (!cloud_config.empty())
            adc_path =
                without_trailing_slash(cloud_config) + "/application_default_credentials.json";
        else
            adc_path = config.default_gcloud_adc_path();
        if (!adc_path.empty()) {
            auto exists = path_exists(adc_path, "Google application credentials");
            if (!exists)
                return std::unexpected(exists.error());
            if (!*exists)
                adc_path.clear();
        }
    }
    if (!adc_path.empty()) {
        auto json = read_text_file(adc_path, "Google application credentials");
        if (!json)
            return std::unexpected(json.error());
        const std::string type = json_string(*json, "type").value_or("");
        if (type == "service_account")
            return gcs_service_account(*json, config, path);
        if (type == "authorized_user")
            return gcs_authorized_user(*json, config, path);
        if (type == "external_account")
            return gcs_external_account(*json, config, path);
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS,
                         "GOOGLE_APPLICATION_CREDENTIALS has unsupported type '" + type + "'"});
    }

    std::string private_key = config.option(path, "GCS_PRIVATE_KEY");
    const std::string private_key_file =
        config.expand_user_path(config.option(path, "GCS_PRIVATE_KEY_FILE"));
    const std::string client_email = config.option(path, "GCS_CLIENT_EMAIL");
    if (!private_key.empty() || !private_key_file.empty() || !client_email.empty()) {
        if (!private_key.empty() && !private_key_file.empty()) {
            return std::unexpected(RequestError{
                KARU_ERR_CREDENTIALS, "set only one of GCS_PRIVATE_KEY and GCS_PRIVATE_KEY_FILE"});
        }
        if (private_key.empty() && !private_key_file.empty()) {
            auto loaded = read_text_file(private_key_file, "GCS OAuth private key");
            if (!loaded)
                return std::unexpected(loaded.error());
            private_key = std::move(*loaded);
        }
        if (private_key.empty() || client_email.empty()) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS,
                             "GCS_CLIENT_EMAIL and a GCS private key must be set together"});
        }
        const std::string scope = config.option(
            path, "GCS_SCOPE", "https://www.googleapis.com/auth/devstorage.read_only");
        return gcs_service_account_values(client_email, normalize_pem(std::move(private_key)),
                                          "https://oauth2.googleapis.com/token", scope,
                                          config.http_options(path));
    }

    const bool metadata_configured = config.has_option(path, "GCS_METADATA_ENDPOINT") ||
                                     config.has_option(path, "GCS_METADATA_DISABLED");
    const bool try_metadata = config.discover_default_credentials() || metadata_configured;
    if (try_metadata && !option_is_true(config.option(path, "GCS_METADATA_DISABLED", "NO"))) {
        const std::string endpoint = config.option(
            path, "GCS_METADATA_ENDPOINT",
            "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/"
            "default/token");
        auto response = credential_request("GET", endpoint, {}, {{"Metadata-Flavor", "Google"}}, 1,
                                           config.http_options(path));
        if (!response && config.has_option(path, "GCS_METADATA_ENDPOINT"))
            return std::unexpected(response.error());
        if (response && (response->status < 200 || response->status >= 300) &&
            config.has_option(path, "GCS_METADATA_ENDPOINT")) {
            return std::unexpected(
                RequestError{KARU_ERR_CREDENTIALS,
                             concat("GCS credential endpoint returned HTTP ", response->status)});
        }
        if (response && response->status >= 200 && response->status < 300) {
            ProviderCredentials result;
            result.bearer_token = json_string(response->body, "access_token").value_or("");
            result.expires_at = static_cast<std::int64_t>(std::time(nullptr)) +
                                json_integer(response->body, "expires_in").value_or(3600);
            if (!result.bearer_token.empty())
                return result;
            if (config.has_option(path, "GCS_METADATA_ENDPOINT")) {
                return std::unexpected(RequestError{
                    KARU_ERR_CREDENTIALS, "GCS credential endpoint response has no access token"});
            }
        }
    }
    return ProviderCredentials{};
}

} // namespace karu::backends
