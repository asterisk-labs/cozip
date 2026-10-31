// C API for positional byte-range reads.
//
// Karu reads local paths, HTTP URLs, and objects in S3, GCS, Azure,
// Hugging Face, and Source Cooperative.
//
// Copyright (c) 2026 Asterisk Labs. MIT.
#ifndef KARU_H
#define KARU_H

#include <stddef.h>
#include <stdint.h>

#define KARU_API

#ifdef __cplusplus
extern "C" {
#endif

#define KARU_API_VERSION 1
// Returned by karu_locator_window_length() for an unbounded locator. This is
// not a valid karu_req.length; range reads must always have a finite length.
#define KARU_TO_END UINT64_MAX
// Use the corresponding client setting in a per-batch options structure.
#define KARU_INHERIT UINT64_MAX

KARU_API int karu_api_version(void);
KARU_API const char* karu_version_string(void);
KARU_API const char* karu_http_backend(void);

typedef enum {
    KARU_OK = 0,
    KARU_END = 1,
    KARU_TIMEOUT = 2,
    KARU_ERR_URI = -1,
    KARU_ERR_UNSUPPORTED = -2,
    KARU_ERR_INVALID = -3,
    KARU_ERR_NOMEM = -4,
    KARU_ERR_IO = -5,
    KARU_ERR_NETWORK = -6,
    KARU_ERR_HTTP = -7,
    KARU_ERR_RANGE = -8,
    KARU_ERR_AUTH = -9,
    KARU_ERR_CANCELLED = -10,
    KARU_ERR_CONFIG = -11,
    KARU_ERR_CREDENTIALS = -12,
    KARU_ERR_NOT_FOUND = -13,
    KARU_ERR_PRECONDITION = -14
} karu_status;

KARU_API const char* karu_status_string(karu_status status);
// Error text is thread-local. Copy it before another failing Karu call on the
// same thread if it must outlive that call.
KARU_API const char* karu_last_error(void);
KARU_API void karu_clear_error(void);
KARU_API void karu_free(void* ptr);

// Configuration

// Configuration builders are mutable and must be externally synchronized.
// Clients copy the configuration and may be shared between threads.
typedef struct karu_config karu_config;
typedef struct karu_client karu_client;

// Initializes a builder from the supported environment variables.
KARU_API karu_status karu_config_create(karu_config** out);
// Initializes an empty builder. Default credential files and metadata
// services are not used unless configured explicitly.
KARU_API karu_status karu_config_create_empty(karu_config** out);
KARU_API void karu_config_free(karu_config* config);

// Option names are case-insensitive. A NULL value removes the option.
KARU_API karu_status karu_config_set_option(karu_config* config, const char* name,
                                            const char* value);

// Sets an option for one path or its descendants. URIs are normalized to VSI
// paths before matching, and the longest matching prefix wins. A NULL value
// removes it. A partial path segment never matches.
KARU_API karu_status karu_config_set_path_option(karu_config* config, const char* prefix,
                                                 const char* name, const char* value);

// Credential callbacks run when a request needs credentials and may run
// concurrently for different paths. They must not re-enter the same client
// and should bound any I/O they perform; Karu cannot interrupt caller code.
// Karu copies all returned strings before the callback returns.
typedef enum {
    KARU_CREDENTIALS_AWS = 1,
    KARU_CREDENTIALS_GCS = 2,
    KARU_CREDENTIALS_AZURE = 3,
    KARU_CREDENTIALS_SOURCE = 4
} karu_credentials_kind;

typedef struct {
    const char* access_key_id;
    const char* secret_access_key;
    const char* session_token;
    const char* bearer_token;
    const char* sas_token;
    const char* account_name;
    // Optional VSI path scope for credential reuse. The scope matches itself
    // and descendants, never a partial segment. NULL or empty disables reuse.
    const char* cache_prefix;
    // Expiration as Unix time, or 0 for credentials without an expiration.
    int64_t expires_at;
} karu_credentials;

typedef karu_status (*karu_credentials_provider)(void* user_data, karu_credentials_kind kind,
                                                 const char* canonical_path, karu_credentials* out);
typedef void (*karu_credentials_provider_free)(void* user_data);

// Pass NULL for provider, user_data, and release to remove the callback.
KARU_API karu_status karu_config_set_credentials_provider(karu_config* config,
                                                          karu_credentials_kind kind,
                                                          karu_credentials_provider provider,
                                                          void* user_data,
                                                          karu_credentials_provider_free release);

// Creates a client from a copy of config. The config may be freed afterwards.
KARU_API karu_status karu_client_create(const karu_config* config, karu_client** out);
KARU_API void karu_client_free(karu_client* client);

// Reports whether config would produce the client's current configuration.
// No I/O is performed.
KARU_API karu_status karu_client_matches_config(const karu_client* client,
                                                const karu_config* config, int* out_match);

KARU_API int karu_client_concurrency(const karu_client* client);
KARU_API uint64_t karu_client_coalesce_gap(const karu_client* client);
KARU_API int karu_client_max_attempts(const karu_client* client);

// Locators

typedef struct karu_locator karu_locator;

// Resolves a local path, supported URI, or VSI path.
KARU_API karu_status karu_resolve(const char* uri, karu_locator** out);
KARU_API void karu_locator_free(karu_locator* locator);
KARU_API const char* karu_locator_uri(const karu_locator* locator);
// Returns nonzero for HTTP and object-storage locators.
KARU_API int karu_locator_is_remote(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_offset(const karu_locator* locator);
KARU_API uint64_t karu_locator_window_length(const karu_locator* locator);

// Gets the size visible through the locator. Results are not cached.
KARU_API karu_status karu_client_size(karu_client* client, const karu_locator* locator,
                                      uint64_t* out_size);

// Size and version of an object. Initialize struct_size to sizeof this struct;
// fields beyond struct_size are left untouched.
typedef struct {
    size_t struct_size;
    // Size visible through the locator, as karu_client_size reports it.
    uint64_t size;
    // Strong ETag, including quotes. Empty for local files, missing or invalid
    // tags, weak tags, and tags longer than 255 bytes. Pass as karu_req.if_match
    // to require this version; a mismatch returns KARU_ERR_PRECONDITION.
    char etag[256];
} karu_object_info;
#define KARU_OBJECT_INFO_INIT                                                                      \
    {                                                                                              \
        sizeof(karu_object_info), 0, { 0 }                                                         \
    }

// Probe size and ETag together. This does not validate format metadata
// supplied by the caller. Unlike karu_client_size, stat probes even a bounded
// remote window to obtain the object's ETag.
KARU_API karu_status karu_client_stat(karu_client* client, const karu_locator* locator,
                                      karu_object_info* out);

// Reads the first head_length and the last tail_length bytes visible through
// the locator, with the size and ETag that karu_client_stat reports. Each
// buffer receives min(length, size) bytes, so the two may overlap on a short
// object. Both ends are requested at once, the tail as a suffix range; Azure
// ignores suffix ranges, so there the tail follows the head. Responses whose
// sizes or strong ETags disagree return KARU_ERR_PRECONDITION. A zero length
// takes a NULL buffer.
KARU_API karu_status karu_client_read_ends(karu_client* client, const karu_locator* locator,
                                           void* head, uint64_t head_length, void* tail,
                                           uint64_t tail_length, karu_object_info* out);

// Positional reads

// Per-batch planner overrides. Initialize struct_size to sizeof this struct.
// Fields set to KARU_INHERIT use the corresponding client setting. A zero gap
// disables coalescing for this batch.
typedef struct {
    size_t struct_size;
    uint64_t coalesce_gap;
    uint64_t coalesce_limit;
    uint64_t coalesce_parts;
    uint64_t coalesce_amplification;
} karu_submit_options;
#define KARU_SUBMIT_OPTIONS_INIT                                                                   \
    { sizeof(karu_submit_options), KARU_INHERIT, KARU_INHERIT, KARU_INHERIT, KARU_INHERIT }

typedef struct {
    const karu_locator* locator;
    uint64_t offset;
    uint64_t length;
    // Destination buffer. If NULL, submit allocates one and the caller must
    // release the completion buffer with karu_free(). Only the first `got`
    // bytes reported by the completion may be read; the rest is unspecified.
    void* buffer;
    void* tag;
    // Optional If-Match condition for remote objects. Copied by submit.
    // A 412 or a response ETag contradicting a single strong tag fails with
    // KARU_ERR_PRECONDITION. Wildcards and lists are evaluated by the server
    // and do not pin a version; interrupted reads with them restart in full.
    const char* if_match;
} karu_req;

typedef struct {
    void* tag;
    karu_status status;
    uint64_t got;
    // Request destination. Only the first `got` bytes may be read. A successful
    // completion reports the requested length; an error normally reports zero,
    // except KARU_ERR_RANGE may report a valid prefix. If Karu allocated the
    // buffer, release it with karu_free() regardless of status.
    void* buffer;
} karu_done;

typedef struct karu_batch karu_batch;

// Submits a batch. Karu copies the requests, locators, and ETags. Destination
// buffers supplied by the caller must remain valid until the batch is freed.
KARU_API karu_status karu_client_submit(karu_client* client, const karu_req* requests, size_t count,
                                        karu_batch** out_batch);
// Submit with planner settings scoped to this batch. Fields not covered by
// struct_size also retain the client setting.
KARU_API karu_status karu_client_submit_with(karu_client* client, const karu_req* requests,
                                             size_t count, const karu_submit_options* options,
                                             karu_batch** out_batch);
// Returns KARU_OK with one completion. Multiple threads may consume the same
// batch. KARU_END and KARU_TIMEOUT leave out unchanged. A batch inherited
// through fork() returns KARU_ERR_INVALID in the child.
KARU_API karu_status karu_batch_next(karu_batch* batch, karu_done* out, int timeout_ms);
// Cancels pending work and waits for active workers to release the buffers.
// Do not call concurrently with karu_batch_next on the same batch. In a forked
// child, an inherited batch is left untouched and the call returns at once.
KARU_API void karu_batch_free(karu_batch* batch);
// Batch counters. Initialize struct_size to sizeof this struct;
// fields beyond struct_size are left untouched.
typedef struct {
    size_t struct_size;
    // Transfers after coalescing, and transfers completed.
    uint64_t transfers;
    uint64_t transfers_finished;
    // Requested bytes exclude invalid requests. Received bytes count local data
    // and accepted response bodies, including gaps, skipped prefixes and retries;
    // HTTP error bodies are excluded.
    uint64_t requested_bytes;
    uint64_t received_bytes;
    // Ordinary retries, and HTTP 429/503 responses. Credential and region
    // corrections do not count as retries.
    uint64_t retries;
    uint64_t throttled;
    // Connections opened rather than reused.
    uint64_t new_connections;
    // Retries planned with additional bytes kept from a partial response.
    uint64_t resumed;
    // Credential refreshes triggered by an authentication failure.
    uint64_t credential_refreshes;
} karu_batch_stats;
#define KARU_BATCH_STATS_INIT                                                                      \
    { sizeof(karu_batch_stats), 0, 0, 0, 0, 0, 0, 0, 0, 0 }

// Reads independent counters, not an atomic snapshot. May be called while
// other threads drain the batch, but not concurrently with karu_batch_free.
KARU_API karu_status karu_batch_get_stats(const karu_batch* batch, karu_batch_stats* out);
// Blocking batch read. Every request must provide a destination buffer. If the
// call fails, the contents of every destination buffer are unspecified.
KARU_API karu_status karu_client_fetch(karu_client* client, const karu_req* requests, size_t count);
KARU_API karu_status karu_client_fetch_with(karu_client* client, const karu_req* requests,
                                            size_t count, const karu_submit_options* options);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KARU_H
