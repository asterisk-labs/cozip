/* cozip 1.1 writer C ABI.
 *
 * Archive names are null-terminated ASCII. Paths are UTF-8. Functions return
 * cozip_status_t and populate err when non-NULL. Outputs are unspecified on
 * failure. See the cozip 1.1 spec for the file format.
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


/* Reserved entry filenames. Profile helpers reject these as user names. */
#define COZIP_INDEX_NAME              "__cozip__"
#define COZIP_INDEX_NAME_LEN          9
#define COZIP_PADDING_NAME            "__cozip_padding__"
#define COZIP_PADDING_NAME_LEN        17
#define COZIP_FLAT_METADATA_NAME      "__metadata__"
#define COZIP_FLAT_METADATA_NAME_LEN  12

/* TACO priority names (spec 14.2). Payloads are not inspected. */
#define COZIP_TACO_COLLECTION_NAME    "COLLECTION.json"
#define COZIP_TACO_METADATA_DIR       "METADATA/"
#define COZIP_TACO_PARQUET_SUFFIX     ".parquet"


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


/* Index profile.
 * NONE: arbitrary priority files.
 * FLAT: one __metadata__ manifest.
 * TACO: final contiguous COLLECTION.json and METADATA Parquet block.
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

/* Caller-owned source; it must outlive the write call. For BUFFER, size must
 * equal the entry payload_size.
 */
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


/* ZIP entry. The caller sets arc_name, payload_size, in_index and source.
 * cozip_plan fills the offsets in place. arc_name is caller-owned ASCII.
 */
typedef struct cozip_entry {
    /* Input. */
    const char     *arc_name;
    uint64_t        payload_size;
    bool            in_index;  /* Include in the byte-0 index. */
    cozip_source_t  source;

    /* Output, filled by cozip_plan. */
    uint64_t lfh_offset;
    uint64_t lfh_size;
    uint64_t payload_offset;
} cozip_entry_t;


/* Path entry for TACO helpers. Strings are caller-owned. Planning fills the
 * offset and, unless supplied by the caller, the size.
 */
typedef struct cozip_path_entry {
    const char *arc_name;
    const char *source_path;
    uint64_t    payload_offset;
    uint64_t    payload_size;
} cozip_path_entry_t;


/* Opaque TACO plan token. Copy unchanged from plan to write. layout_hash
 * covers plan inputs, not file contents.
 */
#define COZIP_TACO_PLAN_VERSION 1u
typedef struct cozip_taco_plan {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t n_files;
    uint64_t n_priorities;
    uint64_t layout_hash;
} cozip_taco_plan_t;

#define COZIP_TACO_PLAN_INIT                                               \
    { (uint32_t)sizeof(cozip_taco_plan_t), COZIP_TACO_PLAN_VERSION, 0, 0, 0 }


/* Fill entry offsets using checked uint64 arithmetic.
 * Returns INVALID_ARGUMENT for invalid names, sizes, counts or overflow.
 */
COZIP_API cozip_status_t cozip_plan(cozip_entry_t *entries, size_t n,
                                    cozip_error_t *err);

/* Return the buffer size required by cozip_build_index_payload. */
COZIP_API cozip_status_t cozip_index_payload_size(const cozip_entry_t *entries,
                                                  size_t n,
                                                  size_t *out_size,
                                                  cozip_error_t *err);

/* Serialize in_index entries into out, preserving entry order.
 * Returns BUFFER_TOO_SMALL when out_size is insufficient.
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


/* Write STORE entries planned by cozip_plan and verify the resulting LFHs and
 * EOCD. The integrity hash remains zero; patch it separately. Invalid inputs
 * are rejected before out_path is replaced. A failed verification removes the
 * new file when possible.
 */
COZIP_API cozip_status_t cozip_write_archive(const char *out_path,
                                             const cozip_entry_t *entries,
                                             size_t n,
                                             const uint8_t *index_payload,
                                             size_t index_payload_size,
                                             cozip_error_t *err);

/* Patch bytes 43..50 with FNV-1a 64 over the index and trailing 32 KiB.
 * Returns ARCHIVE_TOO_SMALL, INVALID_ARGUMENT or IO.
 */
COZIP_API cozip_status_t cozip_patch_integrity_hash(const char *archive_path,
                                                    size_t index_payload_size,
                                                    cozip_error_t *err);


/* Padding helpers for bindings that drive the pipeline directly. */

/* Predicted on-disk size of a planned ZIP32-only archive.
 * `entries` must already have been planned. Returns UINT64_MAX if the
 * calculation overflows or entries is NULL while n is non-zero. */
COZIP_API uint64_t cozip_predict_zip32_archive_size(
    const cozip_entry_t *entries, size_t n, size_t index_payload_size);

/* __cozip_padding__ payload size that lifts `predicted` to
 * COZIP_MIN_ARCHIVE_SIZE. Returns 0 if no padding is needed. */
COZIP_API uint64_t cozip_required_padding_payload(uint64_t predicted);


/* Plan, pad, build the index, write and patch the hash.
 * capacity must be at least n_entries + 1. TACO priority entries must form a
 * final contiguous block and may shift right when padding is inserted.
 */
COZIP_API cozip_status_t cozip_finalize(const char *out_path,
                                        cozip_entry_t *entries,
                                        size_t n_entries,
                                        size_t capacity,
                                        cozip_profile_t profile,
                                        cozip_error_t *err);


/* TACO helpers enforce names, source files and the final priority block.
 * Payload contents remain the caller's responsibility.
 */

/* Plan from data files and priority names. Data paths are required; priority
 * paths may be NULL. Initialize out_plan with COZIP_TACO_PLAN_INIT.
 */
COZIP_API cozip_status_t cozip_plan_taco(
    cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    cozip_taco_plan_t *out_plan,
    cozip_error_t *err);

/* Plan from files[i].payload_size. Source paths are required but not accessed. */
COZIP_API cozip_status_t cozip_plan_taco_sized(
    cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    cozip_taco_plan_t *out_plan,
    cozip_error_t *err);

/* Write from a TACO plan. All sources must exist and match the plan. Priority
 * names keep their planned order. Output/source aliases are rejected.
 */
COZIP_API cozip_status_t cozip_write_taco(
    const char *out_path,
    const cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    const cozip_taco_plan_t *plan,
    cozip_error_t *err);


/* FLAT-profile helpers around cozip_finalize. */

/* Plan FLAT user entries with an __metadata__ placeholder at entries[n_users].
 * Requires capacity >= n_users + 1 and non-zero user payloads.
 */
COZIP_API cozip_status_t cozip_plan_flat(cozip_entry_t *entries,
                                         size_t n_users,
                                         cozip_error_t *err);

/* Write a FLAT archive with metadata_path as __metadata__.
 * Requires capacity >= n_users + 2.
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
