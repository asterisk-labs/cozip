#include "credential_cache.hpp"

#include "backends/contract.hpp"
#include "backends/credentials.hpp"
#include "uri.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <mutex>
#include <new>

namespace karu {
namespace {

constexpr std::int64_t kStaticCredentialTtlSeconds = 60;

template <typename Load>
std::expected<ProviderCredentials, RequestError> load_credentials(Load&& load) noexcept {
    try {
        return load();
    } catch (const std::bad_alloc&) {
        return std::unexpected(RequestError{KARU_ERR_NOMEM, "out of memory loading credentials"});
    } catch (const std::exception& error) {
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS,
                                            std::string("credential provider: ") + error.what()});
    } catch (...) {
        return std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "credential provider raised an unknown exception"});
    }
}

std::string credential_family(std::string_view source, karu_credentials_kind kind) {
    std::string family(source);
    family.push_back('\n');
    family += std::to_string(static_cast<int>(kind));
    family.push_back('\n');
    return family;
}

std::string credential_key(std::string_view family, std::string_view scope) {
    std::string key;
    key.reserve(family.size() + scope.size());
    key.append(family);
    key.append(scope);
    return key;
}

std::string native_credential_key(const ConfigSnapshot& config,
                                  const backends::CloudProvider& provider, std::string_view path) {
    return credential_key(credential_family("native", provider.credentials_kind),
                          config.scope_key(path, provider.credential_options));
}

} // namespace

CredentialCache::CredentialCache(Clock clock) : clock_(std::move(clock)) {}

std::int64_t CredentialCache::now() const {
    return clock_ ? clock_() : static_cast<std::int64_t>(std::time(nullptr));
}

std::int64_t CredentialCache::refresh_time(const ProviderCredentials& credentials, std::int64_t now,
                                           bool refresh_static) noexcept {
    if (credentials.expires_at == 0) {
        return refresh_static ? now + kStaticCredentialTtlSeconds
                              : std::numeric_limits<std::int64_t>::max();
    }
    if (credentials.expires_at <= now)
        return now;
    const std::int64_t lifetime = credentials.expires_at - now;
    const std::int64_t margin = std::clamp<std::int64_t>(lifetime / 10, 1, 60);
    return std::max<std::int64_t>(now + 1, credentials.expires_at - margin);
}

bool CredentialCache::reusable(const Entry& entry, std::int64_t now) noexcept {
    return now < entry.refresh_at &&
           (entry.credentials.expires_at == 0 || now < entry.credentials.expires_at);
}

std::expected<ProviderCredentials, RequestError>
CredentialCache::custom(const ConfigSnapshot& config, karu_credentials_kind kind,
                        std::string_view path) {
    const auto callback = config.provider(kind);
    if (!callback)
        return std::unexpected(RequestError{KARU_ERR_CREDENTIALS, {}});

    const std::int64_t checked_at = now();
    const std::string family = credential_family("custom", kind);
    const auto find_reusable = [&] {
        auto best = entries_.end();
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            const auto& [key, entry] = *iterator;
            const auto& value = entry.credentials;
            const bool current = key.starts_with(family) &&
                                 path_prefix_matches(path, value.cache_prefix) &&
                                 reusable(entry, checked_at);
            if (current &&
                (best == entries_.end() ||
                 value.cache_prefix.size() > best->second.credentials.cache_prefix.size())) {
                best = iterator;
            }
        }
        return best;
    };

    // The callback chooses its cache scope, so the first load can only be
    // keyed by the exact path.
    const std::string flight_key = credential_key(family, path);
    std::shared_ptr<Flight> flight;
    {
        std::unique_lock lock(mutex_);
        const auto found = find_reusable();
        if (found != entries_.end())
            return found->second.credentials;
        if (const auto current = flights_.find(flight_key); current != flights_.end()) {
            auto current_flight = current->second;
            current_flight->ready.wait(lock, [&] { return current_flight->done; });
            return *current_flight->result;
        }
        flight = std::make_shared<Flight>();
        flights_.emplace(flight_key, flight);
    }

    auto refreshed = load_credentials([&] {
        auto value = backends::copy_callback_credentials(callback, kind, path);
        if (value && !value->cache_prefix.empty()) {
            value->cache_prefix = canonical_vsi_path(value->cache_prefix);
            if (!path_prefix_matches(path, value->cache_prefix)) {
                return std::expected<ProviderCredentials, RequestError>{std::unexpected(
                    RequestError{KARU_ERR_CREDENTIALS,
                                 "custom credential cache_prefix does not contain the requested "
                                 "path"})};
            }
        }
        return value;
    });

    const std::int64_t loaded_at = now();
    if (refreshed && refreshed->expires_at != 0 && refreshed->expires_at <= loaded_at) {
        refreshed = std::unexpected(RequestError{
            KARU_ERR_CREDENTIALS, "custom credential provider returned expired credentials"});
    }

    {
        std::unique_lock lock(mutex_);
        // Declared after the lock so the final Flight owner is released while
        // the mutex is still held, including while copying the return value.
        auto completed = std::move(flight);
        if (refreshed && !refreshed->cache_prefix.empty() && !completed->invalidated) {
            try {
                entries_.insert_or_assign(
                    credential_key(family, refreshed->cache_prefix),
                    Entry{*refreshed, refresh_time(*refreshed, loaded_at, false)});
            } catch (...) {
                // A cache insertion must not strand waiters or fail a usable credential load.
            }
        }
        completed->result.emplace(std::move(refreshed));
        completed->done = true;
        const auto current = flights_.find(flight_key);
        if (current != flights_.end() && current->second == completed)
            flights_.erase(current);
        completed->ready.notify_all();
        return *completed->result;
    }
}

std::expected<ProviderCredentials, RequestError>
CredentialCache::native(const ConfigSnapshot& config, const backends::CloudProvider& provider,
                        std::string_view path) {
    const std::string key = native_credential_key(config, provider, path);
    const std::int64_t checked_at = now();
    std::shared_ptr<Flight> flight;
    {
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() && reusable(found->second, checked_at))
            return found->second.credentials;
        if (const auto current = flights_.find(key); current != flights_.end()) {
            auto current_flight = current->second;
            current_flight->ready.wait(lock, [&] { return current_flight->done; });
            return *current_flight->result;
        }
        flight = std::make_shared<Flight>();
        flights_.emplace(key, flight);
    }

    auto loaded = load_credentials([&] { return provider.load_credentials(config, path); });
    const std::int64_t loaded_at = now();
    if (loaded && loaded->expires_at != 0 && loaded->expires_at <= loaded_at) {
        loaded = std::unexpected(
            RequestError{KARU_ERR_CREDENTIALS, "credential provider returned expired credentials"});
    }
    const bool present =
        loaded && (!loaded->access_key_id.empty() || !loaded->secret_access_key.empty() ||
                   !loaded->session_token.empty() || !loaded->bearer_token.empty() ||
                   !loaded->sas_token.empty());
    {
        std::unique_lock lock(mutex_);
        // See custom(): the Flight lifetime remains inside the locked scope.
        auto completed = std::move(flight);
        // Never negative-cache credential discovery. One anonymous/missing lookup
        // must not poison later signed requests.
        if (loaded && present && !completed->invalidated) {
            try {
                entries_.insert_or_assign(key,
                                          Entry{*loaded, refresh_time(*loaded, loaded_at, true)});
            } catch (...) {
                // A cache insertion must not strand waiters or fail a usable credential load.
            }
        }
        completed->result.emplace(std::move(loaded));
        completed->done = true;
        const auto current = flights_.find(key);
        if (current != flights_.end() && current->second == completed)
            flights_.erase(current);
        completed->ready.notify_all();
        return *completed->result;
    }
}

void CredentialCache::invalidate(const ConfigSnapshot& config,
                                 const backends::CloudProvider& provider, std::string_view path,
                                 bool custom) {
    std::unique_lock lock(mutex_);
    // Mark an in-flight lookup too, otherwise its late result could refill the
    // entry rejected by the server.
    if (!custom) {
        const std::string key = native_credential_key(config, provider, path);
        entries_.erase(key);
        if (const auto flight = flights_.find(key); flight != flights_.end()) {
            flight->second->invalidated = true;
            flights_.erase(flight);
        }
        return;
    }

    const std::string family = credential_family("custom", provider.credentials_kind);
    if (const auto flight = flights_.find(credential_key(family, path)); flight != flights_.end()) {
        flight->second->invalidated = true;
        flights_.erase(flight);
    }
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->first.starts_with(family) &&
            path_prefix_matches(path, iterator->second.credentials.cache_prefix)) {
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace karu
