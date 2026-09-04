/*
 * cozip 1.1 writer C ABI, implementation.
 *
 * This file implements the public API declared in cozip.h. The C
 * core only writes archives.
 *
 * The implementation is split into the following sections.
 *
 *   1. Platform shims and includes.
 *   2. Internal constants and build-time guards.
 *   3. Portable 64-bit seek and tell.
 *   4. Version and reserved name accessors.
 *   5. Little-endian byte readers and writers.
 *   6. Error helpers.
 *   7. FNV-1a 64 (internal).
 *   8. Writer-side computation (cozip_plan, payload sizing,
 *      payload serialization, extra field).
 *   9. Disk I/O (hash patching, archive writing).
 *   10. High-level finalize (profile-aware padding + full pipeline).
 *   11. TACO profile (shared path planner + writer).
 *   12. FLAT profile (placeholder plan + finalize wrapper).
 */


/* ---- 1. Platform shims and includes ---- */

#if defined(__linux__) || defined(__gnu_linux__)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#elif defined(__APPLE__) || defined(__MACH__)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#elif defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

/* Forces fseeko/ftello to be 64-bit on glibc, needed for archives
 * larger than 4 GiB.
 */
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

#include "cozip.h"
#include "version.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

#include <sys/types.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#endif


/* ---- 2. Internal constants and build-time guards ---- */

#define LFH_BASE_SIZE          30u
#define ZIP64_LFH_EXTRA_SIZE   20u
#define ZIP32_SIZE_THRESHOLD   0xFFFFFFFFu

/* Read buffer for cozip_patch_integrity_hash. Sized to read the
 * entire 32 KiB suffix region in one fread call.
 */
#define HASH_BUF_SIZE          32768u

/* ZIP record signatures and General Purpose Bit Flag masks used by the
 * post-write verifier (APPNOTE 6.3.10, sections 4.3.7, 4.3.16, 4.4.4).
 */
#define LFH_SIGNATURE          UINT32_C(0x04034B50)
#define EOCD_SIGNATURE         UINT32_C(0x06054B50)
#define EOCD_BASE_SIZE         22u
#define ZIP64_EXTRA_ID         0x0001u
#define ZIP64_LFH_EXTRA_DATA   16u
/* Bits 0 (encrypted), 3 (data descriptor), 6 (strong encryption) and
 * 13 (central directory encryption) are forbidden by spec 5.1.6/5.1.7. */
#define GPBF_FORBIDDEN_MASK    (0x0001u | 0x0008u | 0x0040u | 0x2000u)
/* Largest LFH this writer can emit: base + longest name + ZIP64 extra. */
#define LFH_MAX_SIZE           (LFH_BASE_SIZE + (size_t)UINT16_MAX + ZIP64_LFH_EXTRA_SIZE)

_Static_assert(COZIP_INDEX_OFFSET == 30 + 9 + 12,
               "LFH layout, index payload must start at byte 51");
_Static_assert(COZIP_INDEX_NAME_LEN == sizeof(COZIP_INDEX_NAME) - 1,
               "COZIP_INDEX_NAME_LEN must match COZIP_INDEX_NAME");
_Static_assert(COZIP_FLAT_METADATA_NAME_LEN == sizeof(COZIP_FLAT_METADATA_NAME) - 1,
               "COZIP_FLAT_METADATA_NAME_LEN must match COZIP_FLAT_METADATA_NAME");
_Static_assert(COZIP_PADDING_NAME_LEN == sizeof(COZIP_PADDING_NAME) - 1,
               "COZIP_PADDING_NAME_LEN must match COZIP_PADDING_NAME");
_Static_assert(HASH_BUF_SIZE >= COZIP_HASH_WINDOW_SIZE,
               "hash buffer must hold the full 32 KiB suffix region");
_Static_assert(ZIP64_LFH_EXTRA_SIZE == 4 + ZIP64_LFH_EXTRA_DATA,
               "ZIP64 LFH extra is a 4-byte header plus two u64 sizes");
_Static_assert(COZIP_EXTRA_FIELD_SIZE == 4 + COZIP_EXTRA_DATA_SIZE,
               "0xCA0C extra is a 4-byte header plus an 8-byte hash");


/* ---- 3. Portable 64-bit seek and tell ---- */

#if defined(_MSC_VER)
#  define cozip_fseek64(fp, off) _fseeki64((fp), (__int64)(off), SEEK_SET)
#  define cozip_fseek_end(fp)    _fseeki64((fp), 0, SEEK_END)
#  define cozip_ftell64(fp)      ((long long)_ftelli64(fp))
#else
#  define cozip_fseek64(fp, off) fseeko((fp), (off_t)(off), SEEK_SET)
#  define cozip_fseek_end(fp)    fseeko((fp), 0, SEEK_END)
#  define cozip_ftell64(fp)      ((long long)ftello(fp))
#endif

/* Filesystem paths in the public ABI are UTF-8. libzip already converts its
 * char-path APIs to UTF-16 on Windows; these wrappers do the same for the C
 * runtime calls used before and after libzip.
 */
#if defined(_MSC_VER)
typedef struct __stat64 cozip_stat_t;
#  ifndef S_ISREG
#    define S_ISREG(mode)  (((mode) & _S_IFMT) == _S_IFREG)
#  endif
#elif defined(__MINGW32__)
typedef struct _stat64 cozip_stat_t;
#  ifndef S_ISREG
#    define S_ISREG(mode)  (((mode) & _S_IFMT) == _S_IFREG)
#  endif
#else
typedef struct stat cozip_stat_t;
#endif

#if defined(_WIN32)
static wchar_t *cozip_utf8_to_wide(const char *value) {
    if (!value || strlen(value) > INT_MAX) {
        errno = EINVAL;
        return NULL;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    value, -1, NULL, 0);
    if (count == 0 || (size_t)count > SIZE_MAX / sizeof(wchar_t)) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value, -1, wide, count) == 0) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    return wide;
}

static int cozip_stat(const char *path, cozip_stat_t *st) {
    wchar_t *wide = cozip_utf8_to_wide(path);
    if (!wide) return -1;
    int result = _wstat64(wide, st);
    free(wide);
    return result;
}

static FILE *cozip_fopen(const char *path, const wchar_t *mode) {
    wchar_t *wide = cozip_utf8_to_wide(path);
    if (!wide) return NULL;
    FILE *fp = _wfopen(wide, mode);
    free(wide);
    return fp;
}

#  define COZIP_MODE_RB  L"rb"
#  define COZIP_MODE_RPB L"r+b"

static int cozip_remove(const char *path) {
    wchar_t *wide = cozip_utf8_to_wide(path);
    if (!wide) return -1;
    int result = _wremove(wide);
    free(wide);
    return result;
}

static HANDLE cozip_open_handle(const char *path) {
    wchar_t *wide = cozip_utf8_to_wide(path);
    if (!wide) return INVALID_HANDLE_VALUE;
    HANDLE handle = CreateFileW(
        wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    return handle;
}
#else
static int cozip_stat(const char *path, cozip_stat_t *st) {
    return stat(path, st);
}

static FILE *cozip_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

#  define COZIP_MODE_RB  "rb"
#  define COZIP_MODE_RPB "r+b"

static int cozip_remove(const char *path) {
    return remove(path);
}
#endif

/* ---- 4. Version and reserved name accessors ---- */

COZIP_API const char* cozip_version_string(void) {
    return COZIP_VERSION_STRING;
}

COZIP_API const char *cozip_index_name(void) {
    return COZIP_INDEX_NAME;
}

COZIP_API const char *cozip_padding_name(void) {
    return COZIP_PADDING_NAME;
}

COZIP_API const char *cozip_flat_metadata_name(void) {
    return COZIP_FLAT_METADATA_NAME;
}

COZIP_API const char *cozip_status_string(cozip_status_t status) {
    switch (status) {
        case COZIP_OK:                    return "OK";
        case COZIP_ERR_INVALID_LFH:       return "INVALID_LFH";
        case COZIP_ERR_ARCHIVE_TOO_SMALL: return "ARCHIVE_TOO_SMALL";
        case COZIP_ERR_INVALID_ARGUMENT:  return "INVALID_ARGUMENT";
        case COZIP_ERR_BUFFER_TOO_SMALL:  return "BUFFER_TOO_SMALL";
        case COZIP_ERR_IO:                return "IO";
    }
    return "UNKNOWN";
}

/* ---- 5. Little-endian byte readers and writers ---- */

/* ZIP stores every multi-byte field little-endian on disk. These
 * helpers read and write one byte at a time so the implementation
 * does not depend on the host being little-endian and stays safe
 * on strict-alignment hosts (some ARM cores).
 */

static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p,     (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* ---- 6. Error helpers ---- */

/* Tolerates `err == NULL` so callers that do not care about
 * diagnostics can pass NULL and still get the status code as the
 * return value.
 */
static cozip_status_t set_err(cozip_error_t *err, cozip_status_t code,
                              const char *fmt, ...) {
    if (err) {
        err->code = code;
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(err->message, COZIP_ERROR_MESSAGE_SIZE, fmt, ap);
            va_end(ap);
        } else {
            err->message[0] = '\0';
        }
    }
    return code;
}

static bool ascii_only(const char *value) {
    if (!value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p >= 0x80u) return false;
    }
    return true;
}

static cozip_status_t validate_path(const char *path,
                                    const char *role,
                                    cozip_error_t *err) {
    if (!path || path[0] == '\0') {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "%s must be a non-empty path", role);
    }
    return COZIP_OK;
}

static bool ascii_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static cozip_status_t validate_archive_name_syntax(const char *name,
                                                    size_t position,
                                                    cozip_error_t *err) {
    if (!name) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu has no archive name", position);
    }
    size_t len = strlen(name);
    if (len == 0 || len > UINT16_MAX) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name length must be 1 to 65535 "
                       "bytes (got %zu)",
                       position, len);
    }
    if (!ascii_only(name)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name contains non-ASCII bytes; "
                       "cozip archive names must be ASCII", position);
    }
    if (name[0] == '/') {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name must be relative, not start "
                       "with '/'", position);
    }
    if (len >= 2 && ascii_alpha(name[0]) && name[1] == ':') {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name must not have a drive-letter "
                       "prefix",
                       position);
    }
    if (name[len - 1] == '/') {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name ends with '/'; explicit "
                       "directory entries are not allowed", position);
    }
    if (strchr(name, '\\')) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu archive name contains '\\'; use '/' as "
                       "the separator", position);
    }

    const char *component = name;
    for (const char *p = name;; p++) {
        if (*p != '/' && *p != '\0') continue;
        size_t component_len = (size_t)(p - component);
        if ((component_len == 1 && component[0] == '.') ||
            (component_len == 2 && component[0] == '.' &&
             component[1] == '.')) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu archive name contains a '.' or '..' "
                           "path component", position);
        }
        if (*p == '\0') break;
        component = p + 1;
    }
    return COZIP_OK;
}

static int compare_name_ptrs(const void *left, const void *right) {
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static cozip_status_t validate_entry_names(const cozip_entry_t *entries,
                                           size_t n,
                                           bool indexed_only,
                                           cozip_error_t *err) {
    if (n > SIZE_MAX / sizeof(const char *)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "archive name table allocation would overflow");
    }
    const char **names = n ? (const char **)malloc(n * sizeof(*names)) : NULL;
    if (n && !names) {
        return set_err(err, COZIP_ERR_IO,
                       "archive name table allocation failed");
    }

    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (indexed_only && !entries[i].in_index) continue;
        cozip_status_t s = validate_archive_name_syntax(
            entries[i].arc_name, i, err);
        if (s != COZIP_OK) { free(names); return s; }
        if (strcmp(entries[i].arc_name, COZIP_INDEX_NAME) == 0) {
            free(names);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu uses reserved name '%s'", i,
                           COZIP_INDEX_NAME);
        }
        if (indexed_only &&
            strcmp(entries[i].arc_name, COZIP_PADDING_NAME) == 0) {
            free(names);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "reserved padding entry '%s' must not appear in "
                           "the cozip index", COZIP_PADDING_NAME);
        }
        names[count++] = entries[i].arc_name;
    }

    if (count > 1) qsort(names, count, sizeof(*names), compare_name_ptrs);
    for (size_t i = 1; i < count; i++) {
        if (strcmp(names[i - 1], names[i]) == 0) {
            cozip_status_t s = set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                                       "duplicate archive name '%s'", names[i]);
            free(names);
            return s;
        }
    }
    free(names);
    return COZIP_OK;
}

static cozip_status_t stat_file_size(const char *path, uint64_t *out_size,
                                     cozip_error_t *err);
static bool same_file_as(const char *path, const char *target,
                         const cozip_stat_t *target_st);

/* ---- 7. FNV-1a 64 (internal) ---- */

/* Seed with COZIP_FNV_OFFSET_BASIS for a fresh hash, or with a
 * previous return value to continue across non-contiguous chunks.
 * The hash is order-sensitive; bytes must be presented in
 * archive-byte order.
 */
static uint64_t fnv1a_64(const uint8_t *data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)data[i];
        h *= COZIP_FNV_PRIME;
    }
    return h;
}


/* ---- 8. Writer-side computation ---- */

/* Byte size of a single LFH for an entry with the given name and
 * payload size. Adds a 20-byte ZIP64 extra when the payload size
 * meets or exceeds the ZIP32 sentinel value, mirroring what libzip
 * does for large entries.
 */
static inline uint64_t lfh_size_for(const char *arc_name,
                                    uint64_t payload_size) {
    uint64_t base = LFH_BASE_SIZE + (uint64_t)strlen(arc_name);
    return (payload_size >= ZIP32_SIZE_THRESHOLD) ? base + ZIP64_LFH_EXTRA_SIZE
                                                  : base;
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) return false;
    *out = a + b;
    return true;
}

cozip_status_t cozip_plan(cozip_entry_t *entries, size_t n,
                          cozip_error_t *err) {
    if (n > 0 && !entries) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entries must be non-NULL when n > 0");
    }

    cozip_status_t names_status = validate_entry_names(entries, n, false, err);
    if (names_status != COZIP_OK) return names_status;

    for (size_t i = 0; i < n; i++) {
        if (entries[i].source.kind == COZIP_SOURCE_PATH &&
            entries[i].source.u.path) {
            cozip_status_t s = validate_path(entries[i].source.u.path,
                                             "source path", err);
            if (s != COZIP_OK) return s;
        }
        /* At exactly 0xFFFFFFFF bytes the vendored libzip writes the
         * ZIP32 sentinel in the LFH size fields but an empty ZIP64
         * extra (its data is only emitted for sizes strictly greater
         * than the sentinel). The resulting header is unreadable, so
         * refuse that single size up front instead of failing at the
         * post-write check.
         */
        if (entries[i].payload_size == ZIP32_SIZE_THRESHOLD) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu payload size 0xFFFFFFFF cannot be "
                           "represented in a ZIP local header", i);
        }
    }

    /* The index payload sits between byte 51 and the first user LFH.
     * Its size is fully determined by the names of the in_index
     * entries.
     */
    size_t idx_payload_size = 0;
    cozip_status_t s = cozip_index_payload_size(entries, n,
                                                &idx_payload_size, err);
    if (s != COZIP_OK) return s;

    uint64_t cursor = (uint64_t)COZIP_INDEX_OFFSET
                    + (uint64_t)idx_payload_size;
    for (size_t i = 0; i < n; i++) {
        uint64_t payload_offset = 0;
        uint64_t next = 0;
        uint64_t lfh_size = lfh_size_for(entries[i].arc_name,
                                         entries[i].payload_size);
        if (!add_u64(cursor, lfh_size, &payload_offset) ||
            !add_u64(payload_offset, entries[i].payload_size, &next)) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "archive layout overflows uint64 at entry %zu", i);
        }
        entries[i].lfh_offset     = cursor;
        entries[i].lfh_size       = lfh_size;
        entries[i].payload_offset = payload_offset;
        cursor = next;
    }
    return COZIP_OK;
}

cozip_status_t cozip_index_payload_size(const cozip_entry_t *entries, size_t n,
                                        size_t *out_size,
                                        cozip_error_t *err) {
    if (!out_size || (n > 0 && !entries)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_size and non-empty entries must be non-NULL");
    }

    cozip_status_t names_status = validate_entry_names(entries, n, true, err);
    if (names_status != COZIP_OK) return names_status;

    uint64_t total = COZIP_INDEX_HEADER_SIZE;
    uint64_t n_indexed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        size_t name_len = strlen(entries[i].arc_name);
        if (n_indexed == UINT32_MAX) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "index entry count exceeds UINT32_MAX");
        }
        n_indexed++;
        uint64_t increment = COZIP_INDEX_PER_ENTRY_OVERHEAD
                           + (uint64_t)name_len;
        if (!add_u64(total, increment, &total)) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "index payload size overflows uint64");
        }
    }

    /* The __cozip__ entry is ZIP32 by spec (5.2.1). Its compressed
     * size field is u32, so the index payload cannot reach the
     * 0xFFFFFFFF sentinel.
     */
    if (total >= ZIP32_SIZE_THRESHOLD) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index payload would exceed ZIP32 limit "
                       "(%llu bytes)", (unsigned long long)total);
    }

    *out_size = (size_t)total;
    return COZIP_OK;
}

cozip_status_t cozip_build_index_payload(const cozip_entry_t *entries, size_t n,
                                         cozip_profile_t profile,
                                         uint8_t *out, size_t out_size,
                                         cozip_error_t *err) {
    int profile_value = (int)profile;
    if (profile_value < 0 || profile_value > UINT8_MAX) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "profile must fit in one byte (got %d)", profile_value);
    }

    size_t needed = 0;
    cozip_status_t s = cozip_index_payload_size(entries, n, &needed, err);
    if (s != COZIP_OK) return s;

    if (!out) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index payload output must be non-NULL");
    }
    if (out_size < needed) {
        return set_err(err, COZIP_ERR_BUFFER_TOO_SMALL,
                       "need %zu bytes, got %zu", needed, out_size);
    }

    uint32_t n_indexed = 0;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].in_index) n_indexed++;
    }

    uint8_t *p = out;

    /* Header */
    memcpy(p, COZIP_MAGIC, COZIP_MAGIC_LEN);
    p += COZIP_MAGIC_LEN;
    put_u16(p, COZIP_FORMAT_VERSION); p += 2;
    *p++ = (uint8_t)profile_value;
    put_u32(p, n_indexed); p += 4;

    /* Name lengths */
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        put_u16(p, (uint16_t)strlen(entries[i].arc_name));
        p += 2;
    }
    /* Names, concatenated, no separators */
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        size_t nl = strlen(entries[i].arc_name);
        memcpy(p, entries[i].arc_name, nl);
        p += nl;
    }
    /* Payload offsets */
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        put_u64(p, entries[i].payload_offset);
        p += 8;
    }
    /* Payload sizes */
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        put_u64(p, entries[i].payload_size);
        p += 8;
    }
    return COZIP_OK;
}

void cozip_build_extra_field(uint8_t out[COZIP_EXTRA_FIELD_SIZE]) {
    put_u16(out + 0, COZIP_EXTRA_HEADER_ID);
    put_u16(out + 2, COZIP_EXTRA_DATA_SIZE);
    memset(out + 4, 0, 8);
}


/* ---- 9. Disk I/O ---- */

/* Streams `len` bytes starting at archive offset `start` through
 * the FNV-1a 64 hasher, accumulating into *h. Reads in
 * HASH_BUF_SIZE (32 KiB) chunks so the trailing suffix region is
 * read in one fread call.
 */
static cozip_status_t hash_range(FILE *fp, long long start, size_t len,
                                 uint64_t *h, cozip_error_t *err) {
    uint8_t buf[HASH_BUF_SIZE];

    if (cozip_fseek64(fp, start) != 0) {
        return set_err(err, COZIP_ERR_IO, "seek failed");
    }
    while (len > 0) {
        size_t want = len < sizeof(buf) ? len : sizeof(buf);
        if (fread(buf, 1, want, fp) != want) {
            return set_err(err, COZIP_ERR_IO, "read failed");
        }
        *h = fnv1a_64(buf, want, *h);
        len -= want;
    }
    return COZIP_OK;
}

cozip_status_t cozip_patch_integrity_hash(const char *archive_path,
                                          size_t index_payload_size,
                                          cozip_error_t *err) {
    cozip_status_t path_status = validate_path(
        archive_path, "archive path", err);
    if (path_status != COZIP_OK) return path_status;

    FILE *fp = cozip_fopen(archive_path, COZIP_MODE_RPB);
    if (!fp) {
        return set_err(err, COZIP_ERR_IO,
                       "cannot open '%s'", archive_path);
    }

    if (cozip_fseek_end(fp) != 0) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "seek-end failed");
    }
    long long archive_size = cozip_ftell64(fp);
    if (archive_size < 0) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "tell failed");
    }
    if (archive_size < (long long)COZIP_MIN_ARCHIVE_SIZE) {
        fclose(fp);
        return set_err(err, COZIP_ERR_ARCHIVE_TOO_SMALL,
                       "archive too small (%lld bytes)", archive_size);
    }

    /* Reject inconsistent index_payload_size before any read */
    if (index_payload_size == 0 ||
        index_payload_size >
            (size_t)archive_size - (size_t)COZIP_INDEX_OFFSET) {
        fclose(fp);
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index_payload_size out of range (%zu)",
                       index_payload_size);
    }

    /* 1. Hash the index region */
    uint64_t h = COZIP_FNV_OFFSET_BASIS;
    cozip_status_t s = hash_range(fp, COZIP_INDEX_OFFSET,
                                  index_payload_size, &h, err);
    if (s != COZIP_OK) { fclose(fp); return s; }

    /* 2. Hash the trailing 32 KiB, skipping any overlap with the
     * index region.
     */
    long long suffix_start = archive_size - COZIP_HASH_WINDOW_SIZE;
    long long index_end    = (long long)COZIP_INDEX_OFFSET
                           + (long long)index_payload_size;

    if (index_end <= suffix_start) {
        s = hash_range(fp, suffix_start, COZIP_HASH_WINDOW_SIZE, &h, err);
    } else {
        size_t keep = (size_t)(index_end - suffix_start);
        s = hash_range(fp, index_end,
                       COZIP_HASH_WINDOW_SIZE - keep, &h, err);
    }
    if (s != COZIP_OK) { fclose(fp); return s; }

    /* 3. Patch the 8-byte hash into bytes 43..50 of the first LFH */
    if (cozip_fseek64(fp, 43) != 0) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "seek to hash slot failed");
    }
    uint8_t hb[8];
    put_u64(hb, h);
    if (fwrite(hb, 1, 8, fp) != 8) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "writing hash failed");
    }

    /* fclose flushes the stdio buffer; a failed flush means the hash
     * never reached the file even though fwrite reported success.
     */
    if (fclose(fp) != 0) {
        return set_err(err, COZIP_ERR_IO,
                       "flushing hash to '%s' failed", archive_path);
    }
    return COZIP_OK;
}

/* ---- 9b. Post-write verification ---- */

/* cozip predicts every offset before libzip writes a single byte, and
 * the index plus profile metadata may embed those predictions. If libzip laid
 * an entry out differently, the archive would look valid to ZIP tools
 * while every cozip offset pointed at the wrong bytes. The functions
 * below re-read what landed on disk and compare it field by field with
 * the plan, so a divergence becomes INVALID_LFH instead of silent
 * corruption.
 */

/* Read exactly `len` bytes at `offset`. Short reads are reported as
 * INVALID_LFH: the file is ours and was just closed, so anything
 * shorter than the plan is a layout problem, not a transient I/O one.
 */
static cozip_status_t read_exact(FILE *fp, uint64_t offset,
                                 uint8_t *buf, size_t len,
                                 const char *what, cozip_error_t *err) {
    if (offset > (uint64_t)INT64_MAX || cozip_fseek64(fp, offset) != 0) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: cannot seek to %s at %llu",
                       what, (unsigned long long)offset);
    }
    if (fread(buf, 1, len, fp) != len) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: archive ends before %s at %llu",
                       what, (unsigned long long)offset);
    }
    return COZIP_OK;
}

/* Verify one Local File Header against its planned geometry.
 *
 * `buf` must hold at least LFH_MAX_SIZE bytes. For the __cozip__ entry
 * (`index_entry` true) the extra must be the 12-byte 0xCA0C block; for
 * every other entry it must be empty or, for ZIP64 payloads, the
 * 20-byte 0x0001 record carrying both sizes.
 */
static cozip_status_t verify_lfh(FILE *fp, uint8_t *buf,
                                 const char *arc_name,
                                 uint64_t lfh_offset,
                                 uint64_t lfh_size,
                                 uint64_t payload_size,
                                 bool index_entry,
                                 cozip_error_t *err) {
    size_t name_len = strlen(arc_name);
    if (lfh_size < LFH_BASE_SIZE + name_len || lfh_size > LFH_MAX_SIZE) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: planned LFH size %llu for '%s' "
                       "is out of range", (unsigned long long)lfh_size,
                       arc_name);
    }
    size_t expected_extra = (size_t)lfh_size - LFH_BASE_SIZE - name_len;

    cozip_status_t s = read_exact(fp, lfh_offset, buf, (size_t)lfh_size,
                                  "local file header", err);
    if (s != COZIP_OK) return s;

    if (get_u32(buf) != LFH_SIGNATURE) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: no LFH signature at %llu for '%s'",
                       (unsigned long long)lfh_offset, arc_name);
    }
    uint16_t flags = get_u16(buf + 6);
    if (flags & GPBF_FORBIDDEN_MASK) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: forbidden GP flags 0x%04x on '%s'",
                       (unsigned)flags, arc_name);
    }
    if (get_u16(buf + 8) != 0) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: '%s' is not STORE", arc_name);
    }
    if (get_u16(buf + 26) != name_len || get_u16(buf + 28) != expected_extra) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: name/extra length mismatch on '%s' "
                       "(got %u/%u, planned %zu/%zu)", arc_name,
                       (unsigned)get_u16(buf + 26), (unsigned)get_u16(buf + 28),
                       name_len, expected_extra);
    }
    if (memcmp(buf + LFH_BASE_SIZE, arc_name, name_len) != 0) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: name mismatch at %llu (planned '%s')",
                       (unsigned long long)lfh_offset, arc_name);
    }

    uint32_t comp_size   = get_u32(buf + 18);
    uint32_t uncomp_size = get_u32(buf + 22);
    const uint8_t *extra = buf + LFH_BASE_SIZE + name_len;

    if (index_entry) {
        if (expected_extra != COZIP_EXTRA_FIELD_SIZE ||
            get_u16(extra) != COZIP_EXTRA_HEADER_ID ||
            get_u16(extra + 2) != COZIP_EXTRA_DATA_SIZE) {
            return set_err(err, COZIP_ERR_INVALID_LFH,
                           "post-write check: __cozip__ lacks the 0xCA0C extra");
        }
        if (payload_size >= ZIP32_SIZE_THRESHOLD ||
            comp_size != payload_size || uncomp_size != payload_size) {
            return set_err(err, COZIP_ERR_INVALID_LFH,
                           "post-write check: __cozip__ size fields %u/%u "
                           "do not match index payload %llu",
                           (unsigned)comp_size, (unsigned)uncomp_size,
                           (unsigned long long)payload_size);
        }
        return COZIP_OK;
    }

    if (payload_size >= ZIP32_SIZE_THRESHOLD) {
        if (expected_extra != ZIP64_LFH_EXTRA_SIZE ||
            comp_size != ZIP32_SIZE_THRESHOLD ||
            uncomp_size != ZIP32_SIZE_THRESHOLD ||
            get_u16(extra) != ZIP64_EXTRA_ID ||
            get_u16(extra + 2) != ZIP64_LFH_EXTRA_DATA ||
            get_u64(extra + 4) != payload_size ||
            get_u64(extra + 12) != payload_size) {
            return set_err(err, COZIP_ERR_INVALID_LFH,
                           "post-write check: ZIP64 sizes on '%s' do not "
                           "match planned payload %llu", arc_name,
                           (unsigned long long)payload_size);
        }
        return COZIP_OK;
    }

    if (expected_extra != 0 ||
        comp_size != payload_size || uncomp_size != payload_size) {
        return set_err(err, COZIP_ERR_INVALID_LFH,
                       "post-write check: size fields %u/%u on '%s' do not "
                       "match planned payload %llu",
                       (unsigned)comp_size, (unsigned)uncomp_size, arc_name,
                       (unsigned long long)payload_size);
    }
    return COZIP_OK;
}

/* Re-read the archive libzip just closed and compare it with the plan:
 * the __cozip__ LFH at byte 0, every entry's LFH at its planned offset,
 * the contiguity of the planned layout, and the trailing EOCD with a
 * zero-length comment. Payload bytes are not re-read; libzip's CRC-32
 * already covers them.
 */
static cozip_status_t verify_written_archive(const char *out_path,
                                             const cozip_entry_t *entries,
                                             size_t n,
                                             size_t index_payload_size,
                                             cozip_error_t *err) {
    FILE *fp = cozip_fopen(out_path, COZIP_MODE_RB);
    if (!fp) {
        return set_err(err, COZIP_ERR_IO,
                       "post-write check: cannot reopen '%s'", out_path);
    }
    uint8_t *buf = (uint8_t *)malloc(LFH_MAX_SIZE);
    if (!buf) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO,
                       "post-write check: buffer allocation failed");
    }

    cozip_status_t s = COZIP_OK;
    if (cozip_fseek_end(fp) != 0) {
        s = set_err(err, COZIP_ERR_IO, "post-write check: seek-end failed");
        goto done;
    }
    long long size_ll = cozip_ftell64(fp);
    if (size_ll < 0) {
        s = set_err(err, COZIP_ERR_IO, "post-write check: tell failed");
        goto done;
    }
    uint64_t archive_size = (uint64_t)size_ll;

    /* Index entry: fixed 51-byte LFH at byte 0. */
    s = verify_lfh(fp, buf, COZIP_INDEX_NAME, 0, COZIP_INDEX_OFFSET,
                   (uint64_t)index_payload_size, true, err);
    if (s != COZIP_OK) goto done;

    /* User entries: each LFH must sit exactly where the plan put it,
     * immediately after the previous payload. */
    uint64_t cursor = (uint64_t)COZIP_INDEX_OFFSET + index_payload_size;
    for (size_t i = 0; i < n; i++) {
        const cozip_entry_t *e = &entries[i];
        uint64_t payload_end = 0;
        if (e->lfh_offset != cursor ||
            e->payload_offset != e->lfh_offset + e->lfh_size ||
            !add_u64(e->payload_offset, e->payload_size, &payload_end) ||
            payload_end > archive_size) {
            s = set_err(err, COZIP_ERR_INVALID_LFH,
                        "post-write check: entry %zu ('%s') is not planned "
                        "contiguously at %llu", i, e->arc_name,
                        (unsigned long long)cursor);
            goto done;
        }
        s = verify_lfh(fp, buf, e->arc_name, e->lfh_offset, e->lfh_size,
                       e->payload_size, false, err);
        if (s != COZIP_OK) goto done;
        cursor = payload_end;
    }

    /* EOCD: last 22 bytes, comment length zero (spec 5.1.11). */
    if (archive_size < cursor + EOCD_BASE_SIZE) {
        s = set_err(err, COZIP_ERR_INVALID_LFH,
                    "post-write check: no room for a Central Directory");
        goto done;
    }
    s = read_exact(fp, archive_size - EOCD_BASE_SIZE, buf, EOCD_BASE_SIZE,
                   "EOCD", err);
    if (s != COZIP_OK) goto done;
    if (get_u32(buf) != EOCD_SIGNATURE || get_u16(buf + 20) != 0) {
        s = set_err(err, COZIP_ERR_INVALID_LFH,
                    "post-write check: archive does not end with a "
                    "comment-free EOCD");
        goto done;
    }

done:
    free(buf);
    if (fclose(fp) != 0 && s == COZIP_OK) {
        s = set_err(err, COZIP_ERR_IO,
                    "post-write check: closing '%s' failed", out_path);
    }
    return s;
}

static cozip_status_t remove_failed_output(const char *out_path,
                                           const char *phase,
                                           cozip_status_t failure,
                                           cozip_error_t *err) {
    if (cozip_remove(out_path) == 0 || errno == ENOENT) return failure;
    int remove_errno = errno;
    return set_err(err, COZIP_ERR_IO,
                   "cleanup failed after %s (errno=%d); invalid output may "
                   "remain at '%s'", phase, remove_errno, out_path);
}

cozip_status_t cozip_write_archive(const char *out_path,
                                   const cozip_entry_t *entries, size_t n,
                                   const uint8_t *index_payload,
                                   size_t index_payload_size,
                                   cozip_error_t *err) {
    if (!index_payload || (n > 0 && !entries)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index_payload and non-empty entries "
                       "must be non-NULL");
    }
    cozip_status_t path_status = validate_path(
        out_path, "output path", err);
    if (path_status != COZIP_OK) return path_status;
    /* Pre-flight check on the index payload size. The __cozip__ entry
     * is ZIP32 by spec, so a payload >= 0xFFFFFFFF cannot be written.
     */
    if (index_payload_size == 0 ||
        index_payload_size >= ZIP32_SIZE_THRESHOLD) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index_payload_size out of range (%zu)",
                       index_payload_size);
    }

    cozip_status_t names_status = validate_entry_names(entries, n, false, err);
    if (names_status != COZIP_OK) return names_status;
    names_status = validate_entry_names(entries, n, true, err);
    if (names_status != COZIP_OK) return names_status;

    /* Reject unusable sources before the output is touched. */
    cozip_stat_t out_st;
    const cozip_stat_t *out_st_ptr =
        (cozip_stat(out_path, &out_st) == 0) ? &out_st : NULL;
    for (size_t i = 0; i < n; i++) {
        const cozip_entry_t *e = &entries[i];
        cozip_status_t name_status = validate_archive_name_syntax(
            e->arc_name, i, err);
        if (name_status != COZIP_OK) return name_status;
        if (e->payload_size == 0) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu ('%s') has a zero-byte payload; "
                           "cozip entries must be non-empty", i, e->arc_name);
        }
        if (e->source.kind == COZIP_SOURCE_PATH) {
            if (!e->source.u.path) {
                return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                               "entry %zu ('%s') has no source path",
                               i, e->arc_name);
            }
            uint64_t actual_size = 0;
            path_status = stat_file_size(e->source.u.path, &actual_size, err);
            if (path_status != COZIP_OK) return path_status;
            if (actual_size != e->payload_size) {
                return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                               "entry %zu ('%s') source is %llu bytes, but "
                               "the planned payload size is %llu", i,
                               e->arc_name, (unsigned long long)actual_size,
                               (unsigned long long)e->payload_size);
            }
            if (same_file_as(e->source.u.path, out_path, out_st_ptr)) {
                return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                               "output path is also the source path for "
                               "entry %zu ('%s')", i, e->arc_name);
            }
        } else if (e->source.kind == COZIP_SOURCE_BUFFER) {
            if (!e->source.u.buffer.data ||
                (uint64_t)e->source.u.buffer.size != e->payload_size) {
                return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                               "entry %zu ('%s') buffer size %zu does not "
                               "match payload_size %llu", i, e->arc_name,
                               e->source.u.buffer.size,
                               (unsigned long long)e->payload_size);
            }
        } else {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu ('%s') has no source", i, e->arc_name);
        }
    }

    int zerr = 0;
    zip_t *za = zip_open(out_path, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!za) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, zerr);
        cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                   "zip_open('%s') failed (%s)",
                                   out_path, zip_error_strerror(&ze));
        zip_error_fini(&ze);
        return s;
    }

    /* Phase 1, the __cozip__ index entry */
    zip_source_t *idx_src =
        zip_source_buffer(za, index_payload, index_payload_size, 0);
    if (!idx_src) {
        zip_discard(za);
        return set_err(err, COZIP_ERR_IO,
                       "zip_source_buffer for index failed");
    }
    zip_int64_t idx_id =
        zip_file_add(za, COZIP_INDEX_NAME, idx_src, ZIP_FL_ENC_UTF_8);
    if (idx_id < 0) {
        const char *msg = zip_strerror(za);
        cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                   "zip_file_add(__cozip__) failed (%s)",
                                   msg ? msg : "");
        zip_source_free(idx_src);
        zip_discard(za);
        return s;
    }

    /* The 0xCA0C extra is attached to the LFH only. The hash starts
     * as 8 zero bytes and is patched by cozip_patch_integrity_hash.
     */
    uint8_t zero8[8] = {0};
    if (zip_file_extra_field_set(za, (zip_uint64_t)idx_id,
                                 COZIP_EXTRA_HEADER_ID, ZIP_EXTRA_FIELD_NEW,
                                 zero8, 8, ZIP_FL_LOCAL) < 0) {
        const char *msg = zip_strerror(za);
        cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                   "set 0xCA0C extra failed (%s)",
                                   msg ? msg : "");
        zip_discard(za);
        return s;
    }
    if (zip_set_file_compression(za, (zip_uint64_t)idx_id,
                                 ZIP_CM_STORE, 0) < 0) {
        zip_discard(za);
        return set_err(err, COZIP_ERR_IO,
                       "STORE on __cozip__ failed");
    }

    /* Phase 2, user entries in caller order */
    for (size_t i = 0; i < n; i++) {
        const cozip_entry_t *e = &entries[i];
        zip_source_t *src = NULL;

        if (e->source.kind == COZIP_SOURCE_PATH) {
            src = zip_source_file(za, e->source.u.path, 0,
                                  (zip_int64_t)e->payload_size);
        } else if (e->source.kind == COZIP_SOURCE_BUFFER) {
            src = zip_source_buffer(za, e->source.u.buffer.data,
                                    e->source.u.buffer.size, 0);
        }

        if (!src) {
            const char *msg = zip_strerror(za);
            cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                       "source for '%s' failed (%s)",
                                       e->arc_name, msg ? msg : "");
            zip_discard(za);
            return s;
        }

        zip_int64_t added = zip_file_add(za, e->arc_name, src,
                                         ZIP_FL_ENC_UTF_8);
        if (added < 0) {
            const char *msg = zip_strerror(za);
            cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                       "zip_file_add('%s') failed (%s)",
                                       e->arc_name, msg ? msg : "");
            zip_source_free(src);
            zip_discard(za);
            return s;
        }
        if (zip_set_file_compression(za, (zip_uint64_t)added,
                                     ZIP_CM_STORE, 0) < 0) {
            zip_discard(za);
            return set_err(err, COZIP_ERR_IO,
                           "STORE on '%s' failed", e->arc_name);
        }
    }

    /* Phase 3, finalize the archive. libzip docs require zip_discard
     * after zip_close failure to free the archive struct.
     */
    if (zip_close(za) < 0) {
        const char *msg = zip_strerror(za);
        cozip_status_t s = set_err(err, COZIP_ERR_IO,
                                   "zip_close failed (%s)",
                                   msg ? msg : "");
        zip_discard(za);
        return s;
    }

    /* libzip only renames its temporary file over out_path once
     * zip_close succeeds, so from here on the file at out_path is the
     * one this call produced. If it does not match the plan, remove it
     * rather than leave an archive whose index points at wrong bytes.
     */
    cozip_status_t s = verify_written_archive(out_path, entries, n,
                                              index_payload_size, err);
    if (s != COZIP_OK) {
        return remove_failed_output(out_path, "post-write verification",
                                    s, err);
    }
    return COZIP_OK;
}


/* ---- 10. High-level finalize ---- */

/* TACO requires every indexed priority entry to form the final contiguous
 * entry block. Return the first priority position so optional padding can be
 * inserted immediately before that block without moving any DATA payload.
 *
 * The binding remains responsible for the semantic TACO names and schemas;
 * this check only enforces the physical partition needed by the core writer.
 */
static cozip_status_t taco_priority_start(const cozip_entry_t *entries,
                                          size_t n,
                                          size_t *out_start,
                                          cozip_error_t *err) {
    size_t start = n;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].in_index) {
            start = i;
            break;
        }
    }
    if (start == n) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO profile requires a priority entry block");
    }
    for (size_t i = start; i < n; i++) {
        if (!entries[i].in_index) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "TACO priority entries must be one final "
                           "contiguous block (entry %zu is non-priority)", i);
        }
    }
    *out_start = start;
    return COZIP_OK;
}

/* Central Directory layout sizes from APPNOTE 6.3.10 sections
 * 4.3.12 and 4.3.16. Used only by the padding predictor below.
 */
#define CD_ENTRY_BASE_SIZE  46u
#define EOCD_SIZE           22u

/* 0x5A is ASCII 'Z'; chosen for hexdump readability. The spec
 * does not constrain padding payload contents.
 */
#define COZIP_PADDING_FILL_BYTE  0x5Au

/* Bytes a __cozip_padding__ entry adds to the archive regardless
 * of its payload size: one LFH plus one CD entry, neither
 * carrying extras. The padding payload is small by construction
 * so no ZIP64 LFH extra applies.
 */
#define COZIP_PADDING_ENTRY_OVERHEAD                                       \
    (((uint64_t)LFH_BASE_SIZE      + (uint64_t)COZIP_PADDING_NAME_LEN) +   \
     ((uint64_t)CD_ENTRY_BASE_SIZE + (uint64_t)COZIP_PADDING_NAME_LEN))

COZIP_API uint64_t cozip_predict_zip32_archive_size(const cozip_entry_t *entries,
                                                    size_t n,
                                                    size_t index_payload_size) {
    uint64_t end_of_payloads = 0;
    if (n == 0) {
        if (!add_u64((uint64_t)COZIP_INDEX_OFFSET,
                     (uint64_t)index_payload_size, &end_of_payloads)) {
            return UINT64_MAX;
        }
    } else if (!entries ||
               !add_u64(entries[n - 1].payload_offset,
                        entries[n - 1].payload_size, &end_of_payloads)) {
        return UINT64_MAX;
    }

    uint64_t cd_size = (uint64_t)CD_ENTRY_BASE_SIZE
                     + (uint64_t)COZIP_INDEX_NAME_LEN;
    for (size_t i = 0; i < n; i++) {
        uint64_t entry_size = (uint64_t)CD_ENTRY_BASE_SIZE
                            + (uint64_t)strlen(entries[i].arc_name);
        if (!add_u64(cd_size, entry_size, &cd_size)) return UINT64_MAX;
    }
    uint64_t predicted = 0;
    if (!add_u64(end_of_payloads, cd_size, &predicted) ||
        !add_u64(predicted, (uint64_t)EOCD_SIZE, &predicted)) {
        return UINT64_MAX;
    }
    return predicted;
}

COZIP_API uint64_t cozip_required_padding_payload(uint64_t predicted) {
    if (predicted >= (uint64_t)COZIP_MIN_ARCHIVE_SIZE) return 0;
    uint64_t deficit = (uint64_t)COZIP_MIN_ARCHIVE_SIZE - predicted;
    if (deficit <= COZIP_PADDING_ENTRY_OVERHEAD) return 1;
    return deficit - COZIP_PADDING_ENTRY_OVERHEAD;
}

COZIP_API cozip_status_t cozip_finalize(const char *out_path,
                                        cozip_entry_t *entries,
                                        size_t n_entries,
                                        size_t capacity,
                                        cozip_profile_t profile,
                                        cozip_error_t *err) {
    if (!out_path || !entries) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_path and entries must be non-NULL");
    }
    if (n_entries == SIZE_MAX || capacity < n_entries + 1) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "capacity (%zu) must be at least n_entries + 1 (%zu)",
                       capacity, n_entries == SIZE_MAX ? SIZE_MAX : n_entries + 1);
    }

    size_t padding_position = n_entries;
    if (profile == COZIP_PROFILE_TACO) {
        cozip_status_t s = taco_priority_start(entries, n_entries,
                                               &padding_position, err);
        if (s != COZIP_OK) return s;
    }

    cozip_status_t s = cozip_plan(entries, n_entries, err);
    if (s != COZIP_OK) return s;

    size_t idx_size = 0;
    s = cozip_index_payload_size(entries, n_entries, &idx_size, err);
    if (s != COZIP_OK) return s;

    /* FLAT/NONE append padding as before. TACO inserts it immediately before
     * its final priority block, as required by the profile. In both cases the
     * non-priority DATA entries precede the insertion point, so their offsets
     * remain stable after re-planning. idx_size also stays fixed because the
     * padding entry is never indexed.
     */
    size_t total_n = n_entries;
    uint8_t *padding_buf = NULL;

    uint64_t pad_payload = cozip_required_padding_payload(
        cozip_predict_zip32_archive_size(entries, n_entries, idx_size));

    if (pad_payload > 0) {
        padding_buf = (uint8_t *)malloc((size_t)pad_payload);
        if (!padding_buf) {
            return set_err(err, COZIP_ERR_IO,
                           "padding buffer allocation failed (%llu bytes)",
                           (unsigned long long)pad_payload);
        }
        memset(padding_buf, COZIP_PADDING_FILL_BYTE, (size_t)pad_payload);

        if (padding_position < n_entries) {
            memmove(&entries[padding_position + 1],
                    &entries[padding_position],
                    (n_entries - padding_position) * sizeof(*entries));
        }

        cozip_entry_t *p = &entries[padding_position];
        memset(p, 0, sizeof(*p));
        p->arc_name             = COZIP_PADDING_NAME;
        p->payload_size         = pad_payload;
        p->in_index             = false;
        p->source.kind          = COZIP_SOURCE_BUFFER;
        p->source.u.buffer.data = padding_buf;
        p->source.u.buffer.size = (size_t)pad_payload;
        total_n = n_entries + 1;

        s = cozip_plan(entries, total_n, err);
        if (s != COZIP_OK) { free(padding_buf); return s; }
    }

    uint8_t *idx_buf = (uint8_t *)malloc(idx_size);
    if (!idx_buf) {
        free(padding_buf);
        return set_err(err, COZIP_ERR_IO,
                       "index buffer allocation failed (%zu bytes)",
                       idx_size);
    }
    s = cozip_build_index_payload(entries, total_n, profile,
                                  idx_buf, idx_size, err);
    if (s != COZIP_OK) {
        free(idx_buf);
        free(padding_buf);
        return s;
    }

    /* Both buffers must outlive the call: libzip references them
     * via zip_source_buffer (freep=0) until zip_close runs inside.
     */
    s = cozip_write_archive(out_path, entries, total_n,
                            idx_buf, idx_size, err);
    free(idx_buf);
    free(padding_buf);
    if (s != COZIP_OK) return s;

    /* The archive was written and verified; a failed hash patch would
     * leave a structurally valid ZIP with a zero integrity hash, which
     * cozip-aware readers reject. Remove it so callers never publish it.
     */
    s = cozip_patch_integrity_hash(out_path, idx_size, err);
    if (s != COZIP_OK) {
        return remove_failed_output(out_path, "integrity-hash patch", s, err);
    }
    return COZIP_OK;
}


/* ---- 11. TACO profile ---- */

static cozip_status_t validate_archive_name(const char *name,
                                            size_t position,
                                            cozip_error_t *err) {
    cozip_status_t s = validate_archive_name_syntax(name, position, err);
    if (s != COZIP_OK) return s;
    if (strcmp(name, COZIP_INDEX_NAME) == 0 ||
        strcmp(name, COZIP_PADDING_NAME) == 0) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entry %zu uses reserved name '%s'", position, name);
    }
    return COZIP_OK;
}

/* Spec 14.2: every Parquet file under METADATA/ is a priority file.
 * Matching is by name only; payload contents are not inspected.
 */
static bool taco_metadata_parquet_name(const char *name) {
    size_t len = strlen(name);
    size_t prefix_len = sizeof(COZIP_TACO_METADATA_DIR) - 1;
    size_t suffix_len = sizeof(COZIP_TACO_PARQUET_SUFFIX) - 1;
    if (len < prefix_len + suffix_len) return false;
    return memcmp(name, COZIP_TACO_METADATA_DIR, prefix_len) == 0 &&
           memcmp(name + len - suffix_len, COZIP_TACO_PARQUET_SUFFIX,
                  suffix_len) == 0;
}

static cozip_status_t taco_entry_count(size_t n_files,
                                       size_t n_priorities,
                                       size_t *out_total,
                                       cozip_error_t *err) {
    if (n_priorities == 0) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO requires at least one priority entry");
    }
    if ((uint64_t)n_priorities > UINT32_MAX) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "priority entry count exceeds UINT32_MAX");
    }
    if (n_files > SIZE_MAX - n_priorities) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO entry count overflows size_t");
    }
    *out_total = n_files + n_priorities;
    return COZIP_OK;
}

static cozip_status_t validate_taco_names(
    const cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    size_t *out_total,
    cozip_error_t *err) {
    if (!out_total) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO out_total must be non-NULL");
    }

    cozip_status_t s = taco_entry_count(n_files, n_priorities,
                                        out_total, err);
    if (s != COZIP_OK) return s;
    if ((n_files > 0 && !files) || (n_priorities > 0 && !priorities)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "non-empty TACO entry arrays must be non-NULL");
    }
    if (*out_total == 0) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO entry count must be non-zero");
    }
    if (*out_total > SIZE_MAX / sizeof(const char *)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO name table allocation would overflow");
    }

    const char **names =
        (const char **)malloc(*out_total * sizeof(const char *));
    if (!names) {
        return set_err(err, COZIP_ERR_IO,
                       "TACO name table allocation failed");
    }

    size_t cursor = 0;
    for (size_t i = 0; i < n_files; i++, cursor++) {
        s = validate_archive_name(files[i].arc_name, cursor, err);
        if (s != COZIP_OK) { free(names); return s; }
        names[cursor] = files[i].arc_name;
    }
    bool has_collection = false;
    for (size_t i = 0; i < n_priorities; i++, cursor++) {
        s = validate_archive_name(priorities[i].arc_name, cursor, err);
        if (s != COZIP_OK) { free(names); return s; }
        names[cursor] = priorities[i].arc_name;
        if (strcmp(priorities[i].arc_name, COZIP_TACO_COLLECTION_NAME) == 0) {
            has_collection = true;
        }
    }

    qsort(names, *out_total, sizeof(const char *), compare_name_ptrs);
    for (size_t i = 1; i < *out_total; i++) {
        if (strcmp(names[i - 1], names[i]) == 0) {
            s = set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                        "duplicate archive name '%s'", names[i]);
            free(names);
            return s;
        }
    }
    free(names);

    /* Spec 14.2: the TACO index MUST list COLLECTION.json and every
     * Parquet under METADATA/. Both checks are name-only.
     */
    if (!has_collection) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO profile requires '%s' among the priority files "
                       "(cozip spec 14.2)", COZIP_TACO_COLLECTION_NAME);
    }
    for (size_t i = 0; i < n_files; i++) {
        if (taco_metadata_parquet_name(files[i].arc_name)) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "file %zu ('%s') is a METADATA Parquet and must be "
                           "a priority file (cozip spec 14.2)",
                           i, files[i].arc_name);
        }
    }
    return COZIP_OK;
}

/* Size of a regular file. stat() rather than fopen+fseek so that
 * directories (which fopen happily opens on POSIX), FIFOs (which would
 * block in fopen) and other non-regular sources are rejected up front
 * with a clear message instead of a bogus size or a hang.
 */
static cozip_status_t stat_file_size(const char *path, uint64_t *out_size,
                                     cozip_error_t *err) {
    if (!out_size) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "size output must be non-NULL");
    }
    cozip_status_t path_status = validate_path(path, "source path", err);
    if (path_status != COZIP_OK) return path_status;
    cozip_stat_t st;
    if (cozip_stat(path, &st) != 0) {
        int stat_errno = errno;
        return set_err(err, COZIP_ERR_IO, "cannot stat '%s': %s", path,
                       strerror(stat_errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "'%s' is not a regular file", path);
    }
    if (st.st_size < 0) {
        return set_err(err, COZIP_ERR_IO, "invalid size for '%s'", path);
    }
    *out_size = (uint64_t)st.st_size;
    return COZIP_OK;
}

/* True when `path` and an existing `target` name the same file. String
 * equality works on every platform. POSIX compares device/inode; Windows
 * compares the volume serial and file index returned for open handles.
 * Both catch hard links and different path spellings.
 */
static bool same_file_as(const char *path, const char *target,
                         const cozip_stat_t *target_st) {
    if (strcmp(path, target) == 0) return true;
#if defined(_WIN32)
    if (!target_st) return false;
    HANDLE path_handle = cozip_open_handle(path);
    HANDLE target_handle = cozip_open_handle(target);
    if (path_handle == INVALID_HANDLE_VALUE ||
        target_handle == INVALID_HANDLE_VALUE) {
        if (path_handle != INVALID_HANDLE_VALUE) CloseHandle(path_handle);
        if (target_handle != INVALID_HANDLE_VALUE) CloseHandle(target_handle);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION path_info;
    BY_HANDLE_FILE_INFORMATION target_info;
    bool same = GetFileInformationByHandle(path_handle, &path_info) != 0 &&
                GetFileInformationByHandle(target_handle, &target_info) != 0 &&
                path_info.dwVolumeSerialNumber == target_info.dwVolumeSerialNumber &&
                path_info.nFileIndexHigh == target_info.nFileIndexHigh &&
                path_info.nFileIndexLow == target_info.nFileIndexLow;
    CloseHandle(path_handle);
    CloseHandle(target_handle);
    return same;
#else
    cozip_stat_t st;
    if (target_st && cozip_stat(path, &st) == 0) {
        return st.st_dev == target_st->st_dev && st.st_ino == target_st->st_ino;
    }
#endif
    return false;
}

static cozip_status_t build_taco_entries(
    const cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    bool materialized_priorities,
    cozip_entry_t **out_entries,
    size_t *out_total,
    cozip_error_t *err) {
    if (!out_entries) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_entries must be non-NULL");
    }
    *out_entries = NULL;

    cozip_status_t s = validate_taco_names(files, n_files,
                                           priorities, n_priorities,
                                           out_total, err);
    if (s != COZIP_OK) return s;
    size_t capacity = *out_total;
    if (materialized_priorities) {
        if (capacity == SIZE_MAX) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "TACO scratch capacity overflows size_t");
        }
        capacity++;
    }
    if (capacity > SIZE_MAX / sizeof(cozip_entry_t)) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO entry allocation would overflow");
    }

    cozip_entry_t *entries =
        (cozip_entry_t *)calloc(capacity, sizeof(cozip_entry_t));
    if (!entries) {
        return set_err(err, COZIP_ERR_IO, "TACO entry allocation failed");
    }

    for (size_t i = 0; i < n_files; i++) {
        if (!files[i].source_path) {
            free(entries);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "file %zu has no source path", i);
        }
        uint64_t size = 0;
        s = stat_file_size(files[i].source_path, &size, err);
        if (s != COZIP_OK) { free(entries); return s; }
        if (size == 0) {
            free(entries);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "TACO file %zu ('%s') has a zero-byte source; "
                           "cozip entries must be non-empty",
                           i, files[i].arc_name);
        }
        entries[i].arc_name = files[i].arc_name;
        entries[i].payload_size = size;
        entries[i].in_index = false;
        entries[i].source.kind = COZIP_SOURCE_PATH;
        entries[i].source.u.path = files[i].source_path;
    }

    for (size_t i = 0; i < n_priorities; i++) {
        size_t position = n_files + i;
        entries[position].arc_name = priorities[i].arc_name;
        entries[position].in_index = true;
        if (!materialized_priorities) {
            entries[position].source.kind = COZIP_SOURCE_NONE;
            continue;
        }
        if (!priorities[i].source_path) {
            free(entries);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "priority %zu has no source path", i);
        }
        uint64_t size = 0;
        s = stat_file_size(priorities[i].source_path, &size, err);
        if (s != COZIP_OK) { free(entries); return s; }
        if (size == 0) {
            free(entries);
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "TACO priority %zu ('%s') has a zero-byte source; "
                           "cozip entries must be non-empty",
                           i, priorities[i].arc_name);
        }
        entries[position].payload_size = size;
        entries[position].source.kind = COZIP_SOURCE_PATH;
        entries[position].source.u.path = priorities[i].source_path;
    }

    *out_entries = entries;
    return COZIP_OK;
}

static uint64_t layout_hash_u64(uint64_t value, uint64_t hash) {
    uint8_t bytes[8];
    put_u64(bytes, value);
    return fnv1a_64(bytes, sizeof(bytes), hash);
}

static uint64_t taco_layout_hash(const cozip_entry_t *entries,
                                 size_t n_files,
                                 size_t n_priorities) {
    static const uint8_t domain[] = "cozip-taco-plan-v1";
    uint64_t hash = fnv1a_64(domain, sizeof(domain) - 1,
                             COZIP_FNV_OFFSET_BASIS);
    hash = layout_hash_u64((uint64_t)n_files, hash);
    hash = layout_hash_u64((uint64_t)n_priorities, hash);

    for (size_t i = 0; i < n_files + n_priorities; i++) {
        uint8_t role = i < n_files ? (uint8_t)'D' : (uint8_t)'P';
        hash = fnv1a_64(&role, 1, hash);
        size_t name_len = strlen(entries[i].arc_name);
        hash = layout_hash_u64((uint64_t)name_len, hash);
        hash = fnv1a_64((const uint8_t *)entries[i].arc_name,
                        name_len, hash);
        if (i < n_files) {
            hash = layout_hash_u64(entries[i].payload_size, hash);
        }
    }
    return hash;
}

COZIP_API cozip_status_t cozip_plan_taco(
    cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    cozip_taco_plan_t *out_plan,
    cozip_error_t *err) {
    if (!out_plan) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_plan must be non-NULL");
    }
    if (out_plan->struct_size != sizeof(*out_plan) ||
        out_plan->abi_version != COZIP_TACO_PLAN_VERSION) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "unsupported TACO plan struct size or ABI version");
    }

    cozip_entry_t *entries = NULL;
    size_t total = 0;
    cozip_status_t s = build_taco_entries(files, n_files,
                                          priorities, n_priorities,
                                          false, &entries, &total, err);
    if (s != COZIP_OK) return s;
    if (!entries) {
        return set_err(err, COZIP_ERR_IO,
                       "TACO planner returned no entry storage");
    }

    s = cozip_plan(entries, total, err);
    if (s != COZIP_OK) { free(entries); return s; }

    for (size_t i = 0; i < n_files; i++) {
        files[i].payload_offset = entries[i].payload_offset;
        files[i].payload_size = entries[i].payload_size;
    }
    out_plan->n_files = (uint64_t)n_files;
    out_plan->n_priorities = (uint64_t)n_priorities;
    out_plan->layout_hash = taco_layout_hash(entries, n_files, n_priorities);
    free(entries);
    return COZIP_OK;
}

COZIP_API cozip_status_t cozip_write_taco(
    const char *out_path,
    const cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    const cozip_taco_plan_t *plan,
    cozip_error_t *err) {
    if (!out_path || !plan) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_path and plan must be non-NULL");
    }
    cozip_status_t path_status = validate_path(
        out_path, "output path", err);
    if (path_status != COZIP_OK) return path_status;
    if (plan->struct_size != sizeof(*plan) ||
        plan->abi_version != COZIP_TACO_PLAN_VERSION) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "unsupported TACO plan struct size or ABI version");
    }
    if (plan->n_files != (uint64_t)n_files ||
        plan->n_priorities != (uint64_t)n_priorities) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO entry counts differ from plan");
    }

    cozip_entry_t *entries = NULL;
    size_t total = 0;
    cozip_status_t s = build_taco_entries(files, n_files,
                                          priorities, n_priorities,
                                          true, &entries, &total, err);
    if (s != COZIP_OK) return s;
    if (!entries) {
        return set_err(err, COZIP_ERR_IO,
                       "TACO writer returned no entry storage");
    }

    uint64_t layout_hash = taco_layout_hash(entries, n_files, n_priorities);
    if (layout_hash != plan->layout_hash) {
        free(entries);
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "TACO names/order or file sizes differ from plan");
    }

    s = cozip_plan(entries, total, err);
    if (s != COZIP_OK) { free(entries); return s; }
    for (size_t i = 0; i < n_files; i++) {
        if (entries[i].payload_offset != files[i].payload_offset ||
            entries[i].payload_size != files[i].payload_size) {
            s = set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                        "TACO file %zu layout differs from plan", i);
            free(entries);
            return s;
        }
    }

    /* Never let libzip replace one of its own inputs. When out_path
     * already exists, compare file identity too, not only spelling. */
    cozip_stat_t out_st;
    const cozip_stat_t *out_st_ptr =
        (cozip_stat(out_path, &out_st) == 0) ? &out_st : NULL;
    for (size_t i = 0; i < total; i++) {
        if (same_file_as(entries[i].source.u.path, out_path, out_st_ptr)) {
            s = set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                        "output path is also source path for TACO entry %zu",
                        i);
            free(entries);
            return s;
        }
    }

    s = cozip_finalize(out_path, entries, total, total + 1,
                       COZIP_PROFILE_TACO, err);
    free(entries);
    return s;
}


/* ---- 12. FLAT profile ---- */

/* Configures a single entry slot as a __metadata__ slot. `path` may
 * be NULL for the placeholder used by cozip_plan_flat.
 */
static void setup_flat_metadata_slot(cozip_entry_t *slot,
                                     uint64_t payload_size,
                                     const char *path) {
    memset(slot, 0, sizeof(*slot));
    slot->arc_name     = COZIP_FLAT_METADATA_NAME;
    slot->payload_size = payload_size;
    slot->in_index     = true;
    if (path) {
        slot->source.kind   = COZIP_SOURCE_PATH;
        slot->source.u.path = path;
    } else {
        slot->source.kind = COZIP_SOURCE_NONE;
    }
}

static cozip_status_t reject_flat_reserved_names(const cozip_entry_t *entries,
                                                 size_t n,
                                                 cozip_error_t *err) {
    for (size_t i = 0; i < n; i++) {
        const char *name = entries[i].arc_name;
        if (!name) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu has no archive name", i);
        }
        if (strcmp(name, COZIP_INDEX_NAME) == 0 ||
            strcmp(name, COZIP_FLAT_METADATA_NAME) == 0 ||
            strcmp(name, COZIP_PADDING_NAME) == 0) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu uses FLAT reserved name '%s'", i, name);
        }
    }
    return COZIP_OK;
}

COZIP_API cozip_status_t cozip_plan_flat(cozip_entry_t *entries,
                                         size_t n_users,
                                         cozip_error_t *err) {
    if (!entries) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entries must be non-NULL");
    }
    if (n_users == SIZE_MAX) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "n_users is too large");
    }

    cozip_status_t s = reject_flat_reserved_names(entries, n_users, err);
    if (s != COZIP_OK) return s;

    for (size_t i = 0; i < n_users; i++) {
        if (entries[i].payload_size == 0) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "entry %zu ('%s') has a zero-byte payload; "
                           "FLAT user entries must be non-empty",
                           i, entries[i].arc_name);
        }
        /* Flat has exactly one priority entry: __metadata__. Do not let a
         * stale caller-owned flag silently produce a non-conforming index. */
        entries[i].in_index = false;
    }

    setup_flat_metadata_slot(&entries[n_users], 0, NULL);
    return cozip_plan(entries, n_users + 1, err);
}

COZIP_API cozip_status_t cozip_write_flat(const char *out_path,
                                          cozip_entry_t *entries,
                                          size_t n_users,
                                          size_t capacity,
                                          const char *metadata_path,
                                          cozip_error_t *err) {
    if (!out_path || !entries || !metadata_path) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "out_path, entries and metadata_path must be non-NULL");
    }
    if (n_users > SIZE_MAX - 2 || capacity < n_users + 2) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "capacity (%zu) must be at least n_users + 2 (%zu)",
                       capacity,
                       n_users > SIZE_MAX - 2 ? SIZE_MAX : n_users + 2);
    }

    cozip_status_t s = reject_flat_reserved_names(entries, n_users, err);
    if (s != COZIP_OK) return s;
    for (size_t i = 0; i < n_users; i++) {
        entries[i].in_index = false;
    }

    uint64_t meta_size = 0;
    s = stat_file_size(metadata_path, &meta_size, err);
    if (s != COZIP_OK) return s;

    setup_flat_metadata_slot(&entries[n_users], meta_size, metadata_path);

    return cozip_finalize(out_path, entries, n_users + 1, capacity,
                          COZIP_PROFILE_FLAT, err);
}
