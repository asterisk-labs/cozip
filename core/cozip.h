/*
 * cozip 1.1 writer C ABI.
 *
 * Cloud-Optimized ZIP. The first archive entry is a binary index at
 * byte 0 that lets a reader locate any priority file in one range
 * request, no Central Directory scan.
 *
 * This header is the byte-and-memory layer: offset arithmetic,
 * payload serialization, libzip-driven writes, LFH and index
 * parsing, FNV-1a 64 hashing. Low-level primitives assume validated
 * inputs; profile helpers validate the shared physical rules they own.
 * Dataset semantics remain the bindings' job.
 *
 * Archive names are null-terminated ASCII. Filesystem paths are UTF-8 and are
 * converted to UTF-16 inside the Windows build. Functions manage their own
 * scratch memory. Failures return cozip_status_t and populate *err (err may be NULL).
 * On non-OK, output parameters are unspecified.
 *
 * See the cozip 1.1 spec for the on-disk format.
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

/* TACO profile names (spec 14.2). COLLECTION.json must be a priority
 * file, and so must every "METADATA/<...>.parquet". cozip checks these
 * by name and does not inspect their contents. */
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


/* Profile selector, written into the third byte of the index header.
 *
 *   NONE: any priority files.
 *   FLAT: a single "__metadata__" Parquet manifest.
 *   TACO: "COLLECTION.json" plus every "METADATA/<name>.parquet", contiguous,
 *         placed before the Central Directory.
 *
 * Generic profile finalization only enforces the final priority block. The
 * TACO helpers below also enforce the required priority names. Payload
 * semantics remain the caller's job.
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
 *   arc_name:     null-terminated ASCII, owned by caller, len <= UINT16_MAX.
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


/* Path-only entry used by the shared TACO plan/write helpers.
 *
 * arc_name is null-terminated ASCII. source_path is UTF-8. Both strings are
 * owned by the caller.
 * During cozip_plan_taco, source_path is required for non-priority files and
 * ignored for priority placeholders. The function fills payload_offset and
 * payload_size for each non-priority file.
 */
typedef struct cozip_path_entry {
    const char *arc_name;
    const char *source_path;
    uint64_t    payload_offset;
    uint64_t    payload_size;
} cozip_path_entry_t;


/* Result tying a TACO write to its earlier plan.
 * Bindings should copy this value unchanged from cozip_plan_taco to
 * cozip_write_taco. layout_hash detects changed plan inputs; it does not hash
 * source contents and is not part of the on-disk format.
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


/* Plan the on-disk byte layout using checked u64 arithmetic.
 * Errors: INVALID_ARGUMENT (NULL/empty/non-ASCII/oversized or duplicate names;
 * reserved `__cozip__`; invalid paths; too many indexed entries;
 * oversized index payload,
 * layout arithmetic overflow, or a
 * payload of exactly 0xFFFFFFFF bytes, which libzip cannot encode in a
 * local header). */
COZIP_API cozip_status_t cozip_plan(cozip_entry_t *entries, size_t n,
                                    cozip_error_t *err);

/* Size of the index payload that cozip_build_index_payload would produce.
 * Use the result to size the output buffer.
 *
 * Errors: INVALID_ARGUMENT (NULL output; NULL/empty/non-ASCII/oversized or
 *         duplicate indexed names; reserved `__cozip__`; too many entries;
 *         or payload >= ZIP32 limit; the index entry is
 *         ZIP32 by spec 5.2.1, so it cannot use ZIP64).
 */
COZIP_API cozip_status_t cozip_index_payload_size(const cozip_entry_t *entries,
                                                  size_t n,
                                                  size_t *out_size,
                                                  cozip_error_t *err);

/* Serialize the index payload into `out`. Layout: 11-byte header, then
 * name lengths, names, offsets, sizes. Only in_index=true entries are
 * written, in their order in `entries`. `profile` is recorded in the third
 * header byte and must be between 0 and 255.
 *
 * Errors: same INVALID_ARGUMENT as cozip_index_payload_size, a NULL output,
 *         plus BUFFER_TOO_SMALL.
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
 * validate that what landed matches the plan: the __cozip__ LFH (the
 * first 51 bytes, including the 0xCA0C extra), every entry's LFH at its
 * planned offset (signature, GP flags, STORE method, ZIP32 or ZIP64
 * sizes, name and extra lengths, name bytes), the contiguity of the
 * planned layout, and a trailing comment-free EOCD. `entries` must have
 * been planned with cozip_plan.
 *
 * Every entry uses STORE compression and ASCII names. Zero-byte payloads are
 * rejected before the output is opened. The integrity hash is left zero;
 * call cozip_patch_integrity_hash next.
 *
 * libzip writes to a temporary file and renames it over out_path only on
 * success, so a failure before the post-write check leaves any
 * pre-existing file untouched. If the post-write check fails, cozip removes
 * the new file. A cleanup failure returns IO and warns that the invalid output
 * may remain.
 *
 * Errors: INVALID_ARGUMENT (NULL arguments, invalid names or paths,
 *         zero-byte payload, source.kind == NONE, invalid path source,
 *         source-size drift, output/source alias, NULL buffer, or BUFFER size
 *         mismatch), INVALID_LFH (post-write check failed and cleanup
 *         succeeded), IO.
 */
COZIP_API cozip_status_t cozip_write_archive(const char *out_path,
                                             const cozip_entry_t *entries,
                                             size_t n,
                                             const uint8_t *index_payload,
                                             size_t index_payload_size,
                                             cozip_error_t *err);

/* Compute FNV-1a 64 over (index region) ++ (trailing 32 KiB) and patch
 * bytes 43..50. Overlap on small archives is hashed once. Only those 8
 * bytes are modified; the file is opened r+b and the close is checked,
 * so a failed flush is reported instead of leaving a zero hash.
 *
 * Errors: ARCHIVE_TOO_SMALL, INVALID_ARGUMENT (empty archive path,
 *         zero or oversized index_payload_size), IO.
 */
COZIP_API cozip_status_t cozip_patch_integrity_hash(const char *archive_path,
                                                    size_t index_payload_size,
                                                    cozip_error_t *err);


/* Padding helpers. Used internally by cozip_finalize, exposed for
 * bindings that drive the pipeline step by step. */

/* Predicted on-disk size of a planned ZIP32-only archive.
 * `entries` must already have been planned. Returns UINT64_MAX if the
 * calculation overflows or entries is NULL while n is non-zero. */
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
 * verbatim. For COZIP_PROFILE_TACO, entries marked in_index=true must
 * form one final contiguous block; optional padding is inserted before
 * that block. The function may therefore shift those entries one slot
 * to the right. The generic function does not validate profile filenames or
 * payload semantics.
 *
 * A failure before libzip renames its temporary file leaves any pre-existing
 * file untouched. After the rename, cozip removes an output that fails
 * verification or hash patching. If cleanup itself fails, the function
 * returns IO and the error says that an invalid output may remain.
 */
COZIP_API cozip_status_t cozip_finalize(const char *out_path,
                                        cozip_entry_t *entries,
                                        size_t n_entries,
                                        size_t capacity,
                                        cozip_profile_t profile,
                                        cozip_error_t *err);


/* TACO-profile helpers. Payload contents are opaque to cozip.
 *
 * Both helpers enforce the physical rules of spec Part I and 14.2/14.3
 * that need no payload inspection: portable ASCII names (5.3), no
 * reserved names, archive-wide uniqueness, non-empty regular-file
 * sources, COZIP_TACO_COLLECTION_NAME among the priorities, no
 * "METADATA/<...>.parquet" outside the priorities, and the priorities as
 * one final contiguous block with optional padding before it. JSON and
 * Parquet contents, schemas and the set of METADATA files remain the
 * caller's job.
 */

/* Plan a TACO archive using existing non-priority files and priority names.
 * Priority source paths may be NULL because those files do not need to exist
 * yet. Their names determine the byte-0 index size; their eventual payload
 * sizes cannot move the preceding non-priority files.
 *
 * The caller must initialize *out_plan with COZIP_TACO_PLAN_INIT. On success,
 * every files[i] receives its source size and final payload offset, and
 * *out_plan binds the ordered names and sizes to the later write.
 *
 * Errors: INVALID_ARGUMENT (name rules above, empty or non-regular
 *         source, n_priorities == 0), IO (source cannot be stat'ed).
 */
COZIP_API cozip_status_t cozip_plan_taco(
    cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    cozip_taco_plan_t *out_plan,
    cozip_error_t *err);

/* Write a TACO archive from an earlier plan and materialized priority files.
 * All priority source paths must now exist and be non-empty. The function
 * re-stats every source, recomputes the layout in C, rejects drift from plan,
 * and runs cozip_finalize with COZIP_PROFILE_TACO.
 *
 * Cozip never inspects or changes payload contents. The ordered priority
 * names must be identical to those used for planning. The caller provides
 * no extra-capacity slot; this helper manages all scratch allocation. An
 * output path that names any source (same spelling or the same underlying
 * file when out_path already exists) is rejected before it is opened.
 * Failure semantics are those of cozip_finalize.
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

/* Plan a FLAT archive with a placeholder __metadata__ slot at
 * entries[n_users]. User offsets are stable across the second plan
 * inside cozip_write_flat, so a binding can read them back and write
 * them into the metadata Parquet before that Parquet exists.
 *
 * User entries must have non-zero payload sizes. Their `in_index` fields are
 * set to false so `__metadata__` remains the profile's sole priority entry.
 * `capacity >= n_users + 1`.
 */
COZIP_API cozip_status_t cozip_plan_flat(cozip_entry_t *entries,
                                         size_t n_users,
                                         cozip_error_t *err);

/* Write a FLAT archive given user entries and the path of an
 * already-built metadata Parquet. Configures entries[n_users] for the
 * __metadata__ slot and runs cozip_finalize with COZIP_PROFILE_FLAT.
 *
 * User `in_index` fields are set to false. `capacity >= n_users + 2`.
 * `metadata_path` must remain readable until the call returns.
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
