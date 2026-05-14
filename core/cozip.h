/*
 * cozip 1.0 writer C ABI.
 *
 * Cloud-Optimized ZIP. The first archive entry is a binary index at
 * byte 0 that lets a reader locate any priority file in one range
 * request, no Central Directory scan.
 *
 * This header is the byte-and-memory layer: offset arithmetic,
 * payload serialization, libzip-driven writes, LFH and index
 * parsing, FNV-1a 64 hashing. Format rules above the wire (UTF-8
 * validation, duplicate detection, profile-specific reserved names)
 * are the bindings' job.
 *
 * Strings are null-terminated UTF-8. Pure functions don't allocate;
 * disk-facing ones manage scratch internally. Functions that can
 * fail return cozip_status_t and populate *err (err may be NULL).
 * On non-OK, output parameters are unspecified.
 *
 * See the cozip 1.0 spec for the on-disk format.
 */

#ifndef COZIP_H_
#define COZIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* COZIP_API marks every function in the public ABI. */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef COZIP_BUILDING
    #define COZIP_API __declspec(dllexport)
  #else
    #define COZIP_API __declspec(dllimport)
  #endif
  #define COZIP_LOCAL
#elif defined(__GNUC__) || defined(__clang__)
  #define COZIP_API   __attribute__((visibility("default")))
  #define COZIP_LOCAL __attribute__((visibility("hidden")))
#else
  #define COZIP_API
  #define COZIP_LOCAL
#endif


/* On-disk format version, written into bytes 4-5 of the index payload. */
#define COZIP_FORMAT_VERSION  1

/* Library build version, CalVer (e.g. "2026.5.1.3"). Static, never NULL. */
COZIP_API const char *cozip_version_string(void);


/* Index payload starts at byte 51 = 30 (LFH) + 9 ("__cozip__") + 12 (0xCA0C extra). */
#define COZIP_INDEX_OFFSET      51
/* Integrity hash covers the index region plus the last 32 KiB; archives
 * must be at least COZIP_MIN_ARCHIVE_SIZE bytes long for this to fit. */
#define COZIP_HASH_WINDOW_SIZE  32768
#define COZIP_MIN_ARCHIVE_SIZE  (COZIP_HASH_WINDOW_SIZE + COZIP_INDEX_OFFSET)


/* Reserved entry filenames. Bindings must reject these as user names. */
#define COZIP_INDEX_NAME              "__cozip__"
#define COZIP_INDEX_NAME_LEN          9
#define COZIP_PADDING_NAME            "__cozip_padding__"
#define COZIP_PADDING_NAME_LEN        17
#define COZIP_FLAT_METADATA_NAME      "__metadata__"
#define COZIP_FLAT_METADATA_NAME_LEN  12


/* Index payload magic, first 4 bytes of the payload. */
#define COZIP_MAGIC      "CZIP"
#define COZIP_MAGIC_LEN  4

/* 0xCA0C extra field: header_id(2) + data_size(2) + integrity_hash(8).
 * Only the hash is mutable, and only by cozip_patch_integrity_hash. */
#define COZIP_EXTRA_HEADER_ID   0xCA0Cu
#define COZIP_EXTRA_DATA_SIZE   8u
#define COZIP_EXTRA_FIELD_SIZE  12

/* Index payload header (11 bytes) + per-entry overhead (name_len 2 +
 * offset 8 + size 8); the variable-length name is added on top. */
#define COZIP_INDEX_HEADER_SIZE         11
#define COZIP_INDEX_PER_ENTRY_OVERHEAD  18


/* FNV-1a 64 constants. Exposed so non-C readers (bindings, DuckDB
 * extension, validators) can reproduce the integrity hash. */
#define COZIP_FNV_OFFSET_BASIS  UINT64_C(0xCBF29CE484222325)
#define COZIP_FNV_PRIME         UINT64_C(0x100000001B3)


#define COZIP_ERROR_MESSAGE_SIZE  192


typedef enum cozip_status {
    COZIP_OK = 0,

    /* Format-integrity errors: the archive itself is wrong. */
    COZIP_ERR_INVALID_LFH       = 1,
    COZIP_ERR_ARCHIVE_TOO_SMALL = 2,

    /* Caller-side errors. */
    COZIP_ERR_INVALID_ARGUMENT  = 100,
    COZIP_ERR_BUFFER_TOO_SMALL  = 101,
    COZIP_ERR_IO                = 102
} cozip_status_t;

typedef struct cozip_error {
    cozip_status_t code;
    char           message[COZIP_ERROR_MESSAGE_SIZE];  /* null-terminated UTF-8 */
} cozip_error_t;

/* Stable name for a status, e.g. "INVALID_LFH". Static, never NULL. */
COZIP_API const char *cozip_status_string(cozip_status_t status);


/* Reserved-name accessors, for bindings that prefer ABI calls over macros.
 * Static storage, never NULL. */
COZIP_API const char *cozip_index_name(void);          /* "__cozip__"         */
COZIP_API const char *cozip_padding_name(void);        /* "__cozip_padding__" */
COZIP_API const char *cozip_flat_metadata_name(void);  /* "__metadata__"      */


/* Profile selector, written into the third byte of the index header.
 *
 *   NONE: any priority files.
 *   FLAT: a single "__metadata__" Parquet manifest.
 *   TACO: "COLLECTION.json" plus every "METADATA/*.parquet", contiguous,
 *         placed before the Central Directory.
 *
 * The C core does not enforce profile rules; bindings do.
 */
typedef enum cozip_profile {
    COZIP_PROFILE_NONE = 0,
    COZIP_PROFILE_FLAT = 1,
    COZIP_PROFILE_TACO = 2
} cozip_profile_t;


/* Where cozip_write_archive reads each entry's payload from. */
typedef enum cozip_source_kind {
    COZIP_SOURCE_NONE   = 0,
    COZIP_SOURCE_PATH   = 1,
    COZIP_SOURCE_BUFFER = 2
} cozip_source_kind_t;

/* Tagged union. Caller owns path / buffer; both must outlive the call.
 * For BUFFER, `size` must equal the entry's `payload_size`. */
typedef struct cozip_source {
    cozip_source_kind_t kind;
    union {
        const char *path;
        struct {
            const uint8_t *data;
            size_t         size;
        } buffer;
    } u;
} cozip_source_t;


/* A single ZIP entry. The caller fills the input fields; cozip_plan
 * fills the output fields in place.
 *
 *   arc_name:     null-terminated UTF-8, owned by caller, len <= UINT16_MAX.
 *   payload_size: must match the underlying file or buffer.
 *   in_index:     if false, entry is in the Central Directory but not
 *                 in the cozip index payload.
 *
 * Entries with payload_size > 0xFFFFFFFF get a 20-byte ZIP64 LFH extra,
 * reflected in lfh_size.
 */
typedef struct cozip_entry {
    /* Input. */
    const char     *arc_name;
    uint64_t        payload_size;
    bool            in_index;
    cozip_source_t  source;

    /* Output, filled by cozip_plan. */
    uint64_t lfh_offset;
    uint64_t lfh_size;
    uint64_t payload_offset;
} cozip_entry_t;


/* Plan the on-disk byte layout. Pure arithmetic, always returns OK. */
COZIP_API cozip_status_t cozip_plan(cozip_entry_t *entries, size_t n,
                                    cozip_error_t *err);

/* Size of the index payload that cozip_build_index_payload would produce.
 * Use the result to size the output buffer.
 *
 * Errors: INVALID_ARGUMENT (name > UINT16_MAX, or payload >= ZIP32 limit;
 *         the index entry is ZIP32 by spec 5.2.1, so it cannot use ZIP64).
 */
COZIP_API cozip_status_t cozip_index_payload_size(const cozip_entry_t *entries,
                                                  size_t n,
                                                  size_t *out_size,
                                                  cozip_error_t *err);

/* Serialize the index payload into `out`. Layout: 11-byte header, then
 * name lengths, names, offsets, sizes. Only in_index=true entries are
 * written, in their order in `entries`. `profile` is recorded verbatim
 * in the third header byte; values are not validated.
 *
 * Errors: same INVALID_ARGUMENT as cozip_index_payload_size, plus
 *         BUFFER_TOO_SMALL.
 */
COZIP_API cozip_status_t cozip_build_index_payload(const cozip_entry_t *entries,
                                                   size_t n,
                                                   cozip_profile_t profile,
                                                   uint8_t *out,
                                                   size_t out_size,
                                                   cozip_error_t *err);

/* Write the 12-byte 0xCA0C extra field with the hash zeroed.
 * The hash is patched in later by cozip_patch_integrity_hash. */
COZIP_API void cozip_build_extra_field(uint8_t out[COZIP_EXTRA_FIELD_SIZE]);


/* Write the planned archive to disk via libzip, then re-read and
 * validate that what landed matches the plan: __cozip__ LFH (the first
 * 51 bytes), index size, and every priority entry's LFH (signature, GP
 * flags, STORE method, sizes, name and extra lengths, payload offset).
 *
 * Every entry uses STORE compression and UTF-8 names. The integrity
 * hash is left zero; call cozip_patch_integrity_hash next.
 *
 * Errors: INVALID_ARGUMENT (source.kind == NONE, or BUFFER size doesn't
 *         match payload_size), INVALID_LFH (post-write check failed; the
 *         archive must be considered invalid), IO.
 */
COZIP_API cozip_status_t cozip_write_archive(const char *out_path,
                                             const cozip_entry_t *entries,
                                             size_t n,
                                             const uint8_t *index_payload,
                                             size_t index_payload_size,
                                             cozip_error_t *err);

/* Compute FNV-1a 64 over (index region) ++ (trailing 32 KiB) and patch
 * bytes 43..50. Overlap on small archives is hashed once. Only those 8
 * bytes are modified; the file is opened r+b.
 *
 * Errors: ARCHIVE_TOO_SMALL, INVALID_ARGUMENT (zero or oversized
 *         index_payload_size), IO.
 */
COZIP_API cozip_status_t cozip_patch_integrity_hash(const char *archive_path,
                                                    size_t index_payload_size,
                                                    cozip_error_t *err);


/* Padding helpers. Used internally by cozip_finalize, exposed for
 * bindings that drive the pipeline step by step. */

/* Predicted on-disk size of a planned ZIP32-only archive.
 * `entries` must already have been planned. */
COZIP_API uint64_t cozip_predict_zip32_archive_size(
    const cozip_entry_t *entries, size_t n, size_t index_payload_size);

/* __cozip_padding__ payload size that lifts `predicted` to
 * COZIP_MIN_ARCHIVE_SIZE. Returns 0 if no padding is needed. */
COZIP_API uint64_t cozip_required_padding_payload(uint64_t predicted);


/* Run the full pipeline in one call: plan, padding, build index,
 * write, patch hash.
 *
 * `capacity >= n_entries + 1` to leave room for an optional
 * __cozip_padding__ entry. `profile` is passed to the index payload
 * verbatim; profile-level rules are the bindings' problem.
 *
 * On non-OK, the archive at out_path is unspecified.
 */
COZIP_API cozip_status_t cozip_finalize(const char *out_path,
                                        cozip_entry_t *entries,
                                        size_t n_entries,
                                        size_t capacity,
                                        cozip_profile_t profile,
                                        cozip_error_t *err);


/* FLAT-profile helpers around cozip_finalize. */

/* Plan a FLAT archive with a placeholder __metadata__ slot at
 * entries[n_users]. User offsets are stable across the second plan
 * inside cozip_write_flat, so a binding can read them back and write
 * them into the metadata Parquet before that Parquet exists.
 *
 * `capacity >= n_users + 1`.
 */
COZIP_API cozip_status_t cozip_plan_flat(cozip_entry_t *entries,
                                         size_t n_users,
                                         cozip_error_t *err);

/* Write a FLAT archive given user entries and the path of an
 * already-built metadata Parquet. Configures entries[n_users] for the
 * __metadata__ slot and runs cozip_finalize with COZIP_PROFILE_FLAT.
 *
 * `capacity >= n_users + 2`. `metadata_path` must remain readable
 * until the call returns.
 */
COZIP_API cozip_status_t cozip_write_flat(const char *out_path,
                                          cozip_entry_t *entries,
                                          size_t n_users,
                                          size_t capacity,
                                          const char *metadata_path,
                                          cozip_error_t *err);

#ifdef __cplusplus
}
#endif

#endif