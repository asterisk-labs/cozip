#include "karu/karu.h"

#include "client.hpp"
#include "config.hpp"
#include "error.hpp"
#include "locator.hpp"
#include "runtime/batch.hpp"
#include "runtime/engine.hpp"
#include "text.hpp"
#include "uri.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#ifndef KARU_VERSION_STRING
#define KARU_VERSION_STRING "0.0.0"
#endif

namespace {

karu_status exception_status(std::string_view call) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        karu::set_error(karu::concat(call, ": out of memory"));
        return KARU_ERR_NOMEM;
    } catch (const std::exception& error) {
        karu::set_error(karu::concat(call, ": ", error.what()));
        return KARU_ERR_INVALID;
    } catch (...) {
        karu::set_error(karu::concat(call, ": unknown C++ exception"));
        return KARU_ERR_INVALID;
    }
}

std::vector<karu::Request> convert_requests(const karu_req* requests, std::size_t count,
                                            std::string_view call) {
    std::vector<karu::Request> converted;
    converted.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const karu_req& request = requests[index];
        if (request.locator == nullptr) {
            karu::set_error(karu::concat(call, ": request ", index, " has no locator"));
            return {};
        }
        if (request.length == 0) {
            karu::set_error(karu::concat(call, ": request ", index, " is empty"));
            return {};
        }
        if (request.length == KARU_TO_END) {
            karu::set_error(
                karu::concat(call, ": request ", index, " does not have a finite length"));
            return {};
        }
        if (request.if_match != nullptr &&
            std::string_view(request.if_match).find_first_of("\r\n") != std::string_view::npos) {
            karu::set_error(karu::concat(call, ": request ", index, " has an invalid ETag"));
            return {};
        }
        converted.push_back(karu::Request{request.locator, request.offset, request.length,
                                          request.buffer, request.tag,
                                          request.if_match == nullptr ? "" : request.if_match});
    }
    return converted;
}

void store_object_info(karu_object_info* out, std::uint64_t size, const std::string& etag) {
    out->size = size;
    if (out->struct_size >= offsetof(karu_object_info, etag) + sizeof(out->etag)) {
        // Return an empty ETag rather than a truncated, unusable validator.
        const bool fits = etag.size() < sizeof(out->etag);
        if (fits)
            std::memcpy(out->etag, etag.data(), etag.size());
        out->etag[fits ? etag.size() : 0] = '\0';
    }
}

bool submit_options(const karu::ClientOptions& defaults, const karu_submit_options* overrides,
                    std::string_view call, karu::ClientOptions& result) {
    result = defaults;
    if (overrides == nullptr)
        return true;

    constexpr std::size_t minimum_size =
        offsetof(karu_submit_options, coalesce_gap) + sizeof(std::uint64_t);
    if (overrides->struct_size < minimum_size) {
        karu::set_error(karu::concat(call, ": submit options structure is too small"));
        return false;
    }

    const auto available = [&](std::size_t offset, std::size_t size) {
        return overrides->struct_size >= offset + size;
    };
    const auto inherited = [](std::uint64_t value) { return value == KARU_INHERIT; };

    if (!inherited(overrides->coalesce_gap))
        result.coalesce_gap = overrides->coalesce_gap;

    if (available(offsetof(karu_submit_options, coalesce_limit),
                  sizeof(overrides->coalesce_limit)) &&
        !inherited(overrides->coalesce_limit)) {
        if (overrides->coalesce_limit == 0) {
            karu::set_error(karu::concat(call, ": coalesce_limit must be at least 1"));
            return false;
        }
        result.coalesce_limit = overrides->coalesce_limit;
    }

    if (available(offsetof(karu_submit_options, coalesce_parts),
                  sizeof(overrides->coalesce_parts)) &&
        !inherited(overrides->coalesce_parts)) {
        if (overrides->coalesce_parts == 0 ||
            overrides->coalesce_parts > std::numeric_limits<std::size_t>::max()) {
            karu::set_error(karu::concat(call, ": coalesce_parts is outside the supported range"));
            return false;
        }
        result.coalesce_parts = static_cast<std::size_t>(overrides->coalesce_parts);
    }

    if (available(offsetof(karu_submit_options, coalesce_amplification),
                  sizeof(overrides->coalesce_amplification)) &&
        !inherited(overrides->coalesce_amplification)) {
        if (overrides->coalesce_amplification == 0) {
            karu::set_error(karu::concat(call, ": coalesce_amplification must be at least 1"));
            return false;
        }
        result.coalesce_amplification = overrides->coalesce_amplification;
    }
    return true;
}

karu_status client_submit(karu_client* client, const karu_req* requests, std::size_t count,
                          const karu_submit_options* options, karu_batch** out_batch,
                          std::string_view call) {
    if (client == nullptr || out_batch == nullptr || (count > 0 && requests == nullptr)) {
        karu::set_error(karu::concat(call, ": null argument"));
        return KARU_ERR_INVALID;
    }
    *out_batch = nullptr;

    const auto converted = convert_requests(requests, count, call);
    if (converted.size() != count)
        return KARU_ERR_INVALID;
    karu::ClientOptions plan_options;
    if (!submit_options(client->config.client_options(), options, call, plan_options))
        return KARU_ERR_INVALID;

    auto engine = client->acquire_engine();
    karu_status status = KARU_OK;
    auto batch = engine->submit(converted, plan_options, status);
    if (status != KARU_OK)
        return status;
    batch->owner = std::move(engine);
    *out_batch = batch.release();
    return KARU_OK;
}

karu_status client_fetch(karu_client* client, const karu_req* requests, std::size_t count,
                         const karu_submit_options* options, std::string_view call) {
    if (client == nullptr || (count > 0 && requests == nullptr)) {
        karu::set_error(karu::concat(call, ": null argument"));
        return KARU_ERR_INVALID;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (requests[index].buffer == nullptr) {
            karu::set_error(karu::concat(call, ": request ", index, " has no destination buffer"));
            return KARU_ERR_INVALID;
        }
    }

    karu_batch* submitted = nullptr;
    const karu_status status = client_submit(client, requests, count, options, &submitted, call);
    if (status != KARU_OK)
        return status;
    const std::unique_ptr<karu_batch, decltype(&karu_batch_free)> batch(submitted, karu_batch_free);

    karu_status first_failure = KARU_OK;
    std::string first_detail;
    for (;;) {
        karu::Completion completion;
        const karu_status step = batch->next(completion, -1);
        if (step == KARU_END)
            break;
        if (step != KARU_OK) {
            first_failure = step;
            break;
        }
        if (first_failure == KARU_OK && completion.status != KARU_OK) {
            first_failure = completion.status;
            first_detail = completion.detail;
        }
    }
    if (!first_detail.empty())
        karu::set_error(first_detail);
    return first_failure;
}

} // namespace

extern "C" {

int karu_api_version(void) {
    return KARU_API_VERSION;
}

const char* karu_version_string(void) {
    return KARU_VERSION_STRING;
}

const char* karu_http_backend(void) {
    return curl_version();
}

const char* karu_status_string(karu_status status) {
    switch (status) {
    case KARU_OK:
        return "ok";
    case KARU_END:
        return "batch drained";
    case KARU_TIMEOUT:
        return "timed out";
    case KARU_ERR_URI:
        return "malformed URI";
    case KARU_ERR_UNSUPPORTED:
        return "unsupported virtual filesystem";
    case KARU_ERR_INVALID:
        return "invalid argument";
    case KARU_ERR_NOMEM:
        return "out of memory";
    case KARU_ERR_IO:
        return "file error";
    case KARU_ERR_NETWORK:
        return "network error";
    case KARU_ERR_HTTP:
        return "http error";
    case KARU_ERR_RANGE:
        return "range past end of object";
    case KARU_ERR_AUTH:
        return "not authorized";
    case KARU_ERR_CANCELLED:
        return "cancelled";
    case KARU_ERR_CONFIG:
        return "invalid configuration";
    case KARU_ERR_CREDENTIALS:
        return "credentials unavailable";
    case KARU_ERR_NOT_FOUND:
        return "object not found";
    case KARU_ERR_PRECONDITION:
        return "precondition failed";
    }
    return "unknown status";
}

const char* karu_last_error(void) {
    return karu::last_error();
}

void karu_clear_error(void) {
    karu::clear_error();
}

void karu_free(void* pointer) {
    std::free(pointer);
}

karu_status karu_config_create(karu_config** out) {
    karu::clear_error();
    if (out == nullptr) {
        karu::set_error("karu_config_create: null output");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        *out = new karu_config(true);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_create");
    }
}

karu_status karu_config_create_empty(karu_config** out) {
    karu::clear_error();
    if (out == nullptr) {
        karu::set_error("karu_config_create_empty: null output");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        *out = new karu_config(false);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_create_empty");
    }
}

void karu_config_free(karu_config* config) {
    delete config;
}

karu_status karu_config_set_option(karu_config* config, const char* name, const char* value) {
    karu::clear_error();
    if (config == nullptr || name == nullptr) {
        karu::set_error("karu_config_set_option: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set(name, value);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_option");
    }
}

karu_status karu_config_set_path_option(karu_config* config, const char* prefix, const char* name,
                                        const char* value) {
    karu::clear_error();
    if (config == nullptr || prefix == nullptr || name == nullptr) {
        karu::set_error("karu_config_set_path_option: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set_path(prefix, name, value);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_path_option");
    }
}

karu_status karu_config_set_credentials_provider(karu_config* config, karu_credentials_kind kind,
                                                 karu_credentials_provider provider,
                                                 void* user_data,
                                                 karu_credentials_provider_free release) {
    karu::clear_error();
    if (config == nullptr) {
        karu::set_error("karu_config_set_credentials_provider: null config");
        return KARU_ERR_INVALID;
    }
    try {
        auto result = config->builder.set_provider(kind, provider, user_data, release);
        if (!result) {
            karu::set_error(result.error());
            return KARU_ERR_CONFIG;
        }
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_config_set_credentials_provider");
    }
}

karu_status karu_client_create(const karu_config* config, karu_client** out) {
    karu::clear_error();
    if (config == nullptr || out == nullptr) {
        karu::set_error("karu_client_create: null argument");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        auto snapshot = config->builder.freeze();
        if (!snapshot) {
            karu::set_error(snapshot.error());
            return KARU_ERR_CONFIG;
        }
        *out = new karu_client(std::move(*snapshot));
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_create");
    }
}

void karu_client_free(karu_client* client) {
    delete client;
}

karu_status karu_client_matches_config(const karu_client* client, const karu_config* config,
                                       int* out_match) {
    karu::clear_error();
    if (client == nullptr || config == nullptr || out_match == nullptr) {
        karu::set_error("karu_client_matches_config: null argument");
        return KARU_ERR_INVALID;
    }
    *out_match = 0;
    try {
        auto snapshot = config->builder.freeze();
        if (!snapshot) {
            karu::set_error(snapshot.error());
            return KARU_ERR_CONFIG;
        }
        *out_match = client->config == *snapshot;
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_matches_config");
    }
}

int karu_client_concurrency(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().concurrency;
}

uint64_t karu_client_coalesce_gap(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().coalesce_gap;
}

int karu_client_max_attempts(const karu_client* client) {
    return client == nullptr ? 0 : client->config.client_options().max_attempts;
}

karu_status karu_resolve(const char* uri, karu_locator** out) {
    karu::clear_error();
    if (uri == nullptr || out == nullptr) {
        karu::set_error("karu_resolve: null argument");
        return KARU_ERR_INVALID;
    }
    *out = nullptr;
    try {
        auto resolved = karu::resolve(uri);
        if (!resolved) {
            karu::set_error(resolved.error().message);
            return resolved.error().code == karu::ResolveErrorCode::Unsupported
                       ? KARU_ERR_UNSUPPORTED
                       : KARU_ERR_URI;
        }
        *out = new karu::Locator{std::move(*resolved)};
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_resolve");
    }
}

void karu_locator_free(karu_locator* locator) {
    delete locator;
}

const char* karu_locator_uri(const karu_locator* locator) {
    return locator == nullptr ? "" : locator->resolved.canonical_uri.c_str();
}

int karu_locator_is_remote(const karu_locator* locator) {
    return locator != nullptr && locator->resolved.backend != karu::Backend::File;
}

uint64_t karu_locator_window_offset(const karu_locator* locator) {
    return locator == nullptr ? 0 : locator->resolved.window_offset;
}

uint64_t karu_locator_window_length(const karu_locator* locator) {
    return locator == nullptr ? 0 : locator->resolved.window_length;
}

karu_status karu_client_size(karu_client* client, const karu_locator* locator, uint64_t* out_size) {
    karu::clear_error();
    if (client == nullptr || locator == nullptr || out_size == nullptr) {
        karu::set_error("karu_client_size: null argument");
        return KARU_ERR_INVALID;
    }
    try {
        return client->acquire_engine()->size_of(*locator, *out_size);
    } catch (...) {
        return exception_status("karu_client_size");
    }
}

karu_status karu_client_stat(karu_client* client, const karu_locator* locator,
                             karu_object_info* out) {
    karu::clear_error();
    if (client == nullptr || locator == nullptr || out == nullptr) {
        karu::set_error("karu_client_stat: null argument");
        return KARU_ERR_INVALID;
    }
    if (out->struct_size < offsetof(karu_object_info, size) + sizeof(out->size)) {
        karu::set_error("karu_client_stat: struct_size is too small");
        return KARU_ERR_INVALID;
    }
    try {
        std::uint64_t size = 0;
        std::string etag;
        const karu_status status = client->acquire_engine()->object_info(*locator, size, etag);
        if (status != KARU_OK)
            return status;
        store_object_info(out, size, etag);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_stat");
    }
}

karu_status karu_client_read_ends(karu_client* client, const karu_locator* locator, void* head,
                                  uint64_t head_length, void* tail, uint64_t tail_length,
                                  karu_object_info* out) {
    karu::clear_error();
    if (client == nullptr || locator == nullptr || out == nullptr ||
        (head == nullptr && head_length > 0) || (tail == nullptr && tail_length > 0)) {
        karu::set_error("karu_client_read_ends: null argument");
        return KARU_ERR_INVALID;
    }
    if (out->struct_size < offsetof(karu_object_info, size) + sizeof(out->size)) {
        karu::set_error("karu_client_read_ends: struct_size is too small");
        return KARU_ERR_INVALID;
    }
    if (head_length == KARU_TO_END || tail_length == KARU_TO_END ||
        head_length > std::numeric_limits<std::size_t>::max() ||
        tail_length > std::numeric_limits<std::size_t>::max()) {
        karu::set_error("karu_client_read_ends: lengths must be finite");
        return KARU_ERR_INVALID;
    }
    try {
        std::uint64_t size = 0;
        std::string etag;
        const karu_status status = client->acquire_engine()->read_ends(
            *locator, {static_cast<std::byte*>(head), static_cast<std::size_t>(head_length)},
            {static_cast<std::byte*>(tail), static_cast<std::size_t>(tail_length)}, size, etag);
        if (status != KARU_OK)
            return status;
        store_object_info(out, size, etag);
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_client_read_ends");
    }
}

karu_status karu_client_submit(karu_client* client, const karu_req* requests, size_t count,
                               karu_batch** out_batch) {
    karu::clear_error();
    try {
        return client_submit(client, requests, count, nullptr, out_batch, "karu_client_submit");
    } catch (...) {
        return exception_status("karu_client_submit");
    }
}

karu_status karu_client_submit_with(karu_client* client, const karu_req* requests, size_t count,
                                    const karu_submit_options* options, karu_batch** out_batch) {
    karu::clear_error();
    try {
        return client_submit(client, requests, count, options, out_batch,
                             "karu_client_submit_with");
    } catch (...) {
        return exception_status("karu_client_submit_with");
    }
}

karu_status karu_batch_next(karu_batch* batch, karu_done* out, int timeout_ms) {
    karu::clear_error();
    if (batch == nullptr || out == nullptr) {
        karu::set_error("karu_batch_next: null argument");
        return KARU_ERR_INVALID;
    }
    if (batch->inherited()) {
        karu::set_error("karu_batch_next: the batch was submitted by another process; "
                        "batches do not cross fork()");
        return KARU_ERR_INVALID;
    }
    try {
        karu::Completion completion;
        const karu_status status = batch->next(completion, timeout_ms);
        if (status != KARU_OK)
            return status;
        if (completion.status != KARU_OK && !completion.detail.empty())
            karu::set_error(completion.detail);
        out->tag = completion.tag;
        out->status = completion.status;
        out->got = completion.got;
        out->buffer = completion.release_buffer();
        return KARU_OK;
    } catch (...) {
        return exception_status("karu_batch_next");
    }
}

karu_status karu_batch_get_stats(const karu_batch* batch, karu_batch_stats* out) {
    karu::clear_error();
    if (batch == nullptr || out == nullptr) {
        karu::set_error("karu_batch_get_stats: null argument");
        return KARU_ERR_INVALID;
    }
    if (batch->inherited()) {
        karu::set_error("karu_batch_get_stats: the batch was submitted by another process; "
                        "batches do not cross fork()");
        return KARU_ERR_INVALID;
    }
    if (out->struct_size < offsetof(karu_batch_stats, transfers) + sizeof(out->transfers)) {
        karu::set_error("karu_batch_get_stats: struct_size is too small");
        return KARU_ERR_INVALID;
    }
    const auto& counters = batch->counters;
    const auto copy = [out](std::size_t offset, std::uint64_t& field,
                            const std::atomic<std::uint64_t>& counter) {
        if (out->struct_size >= offset + sizeof(field))
            field = counter.load(std::memory_order_relaxed);
    };
    copy(offsetof(karu_batch_stats, transfers), out->transfers, counters.transfers);
    copy(offsetof(karu_batch_stats, transfers_finished), out->transfers_finished,
         counters.transfers_finished);
    copy(offsetof(karu_batch_stats, requested_bytes), out->requested_bytes,
         counters.requested_bytes);
    copy(offsetof(karu_batch_stats, received_bytes), out->received_bytes, counters.received_bytes);
    copy(offsetof(karu_batch_stats, retries), out->retries, counters.retries);
    copy(offsetof(karu_batch_stats, throttled), out->throttled, counters.throttled);
    copy(offsetof(karu_batch_stats, new_connections), out->new_connections,
         counters.new_connections);
    copy(offsetof(karu_batch_stats, resumed), out->resumed, counters.resumed);
    copy(offsetof(karu_batch_stats, credential_refreshes), out->credential_refreshes,
         counters.credential_refreshes);
    return KARU_OK;
}

void karu_batch_free(karu_batch* batch) {
    if (batch == nullptr)
        return;
    if (batch->inherited()) {
        // The child has no workers to finish this batch and may inherit a
        // locked mutex. Leave it allocated until the child exits.
        return;
    }
    std::unique_ptr<karu::BatchCore> owned(batch);
    const auto engine = owned->owner;
    if (engine)
        engine->cancel(*owned);
}

karu_status karu_client_fetch(karu_client* client, const karu_req* requests, size_t count) {
    karu::clear_error();
    try {
        return client_fetch(client, requests, count, nullptr, "karu_client_fetch");
    } catch (...) {
        return exception_status("karu_client_fetch");
    }
}

karu_status karu_client_fetch_with(karu_client* client, const karu_req* requests, size_t count,
                                   const karu_submit_options* options) {
    karu::clear_error();
    try {
        return client_fetch(client, requests, count, options, "karu_client_fetch_with");
    } catch (...) {
        return exception_status("karu_client_fetch_with");
    }
}

} // extern "C"
