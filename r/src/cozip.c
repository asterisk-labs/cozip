/*
 * cozip 1.0 writer C ABI, implementation.
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
 *   10. High-level finalize (padding decision + full pipeline).
 *   11. FLAT profile (placeholder plan + finalize wrapper).
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

#ifndef _MSC_VER
#  include <sys/types.h>
#endif


/* ---- 2. Internal constants and build-time guards ---- */

#define LFH_BASE_SIZE          30u
#define ZIP64_LFH_EXTRA_SIZE   20u
#define ZIP32_SIZE_THRESHOLD   0xFFFFFFFFu

/* Read buffer for cozip_patch_integrity_hash. Sized to read the
 * entire 32 KiB suffix region in one fread call.
 */
#define HASH_BUF_SIZE          32768u

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

cozip_status_t cozip_plan(cozip_entry_t *entries, size_t n,
                          cozip_error_t *err) {
    (void)err;

    /* The index payload sits between byte 51 and the first user LFH.
     * Its size is fully determined by the names of the in_index
     * entries.
     */
    uint64_t idx_payload_size = COZIP_INDEX_HEADER_SIZE;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].in_index) {
            idx_payload_size += COZIP_INDEX_PER_ENTRY_OVERHEAD
                              + (uint64_t)strlen(entries[i].arc_name);
        }
    }

    uint64_t cursor = (uint64_t)COZIP_INDEX_OFFSET + idx_payload_size;
    for (size_t i = 0; i < n; i++) {
        entries[i].lfh_offset     = cursor;
        entries[i].lfh_size       = lfh_size_for(entries[i].arc_name,
                                                 entries[i].payload_size);
        entries[i].payload_offset = cursor + entries[i].lfh_size;
        cursor = entries[i].payload_offset + entries[i].payload_size;
    }
    return COZIP_OK;
}

cozip_status_t cozip_index_payload_size(const cozip_entry_t *entries, size_t n,
                                        size_t *out_size,
                                        cozip_error_t *err) {
    uint64_t total = COZIP_INDEX_HEADER_SIZE;
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].in_index) continue;
        size_t name_len = strlen(entries[i].arc_name);
        if (name_len > UINT16_MAX) {
            return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                           "arc_name longer than UINT16_MAX on entry %zu "
                           "(%zu bytes)", i, name_len);
        }
        total += COZIP_INDEX_PER_ENTRY_OVERHEAD + (uint64_t)name_len;
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
    size_t needed = 0;
    cozip_status_t s = cozip_index_payload_size(entries, n, &needed, err);
    if (s != COZIP_OK) return s;

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
    *p++ = (uint8_t)profile;
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
    FILE *fp = fopen(archive_path, "r+b");
    if (!fp) {
        return set_err(err, COZIP_ERR_IO,
                       "cannot open '%s'", archive_path);
    }

    if (cozip_fseek_end(fp) != 0) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "seek-end failed");
    }
    long long archive_size = cozip_ftell64(fp);
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

    fclose(fp);
    return COZIP_OK;
}

cozip_status_t cozip_write_archive(const char *out_path,
                                   const cozip_entry_t *entries, size_t n,
                                   const uint8_t *index_payload,
                                   size_t index_payload_size,
                                   cozip_error_t *err) {
    /* Pre-flight check on the index payload size. The __cozip__ entry
     * is ZIP32 by spec, so a payload >= 0xFFFFFFFF cannot be written.
     */
    if (index_payload_size == 0 ||
        index_payload_size >= ZIP32_SIZE_THRESHOLD) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "index_payload_size out of range (%zu)",
                       index_payload_size);
    }

    int zerr = 0;
    zip_t *za = zip_open(out_path, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!za) {
        return set_err(err, COZIP_ERR_IO,
                       "zip_open failed (libzip err=%d)", zerr);
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
        zip_source_free(idx_src);
        zip_discard(za);
        return set_err(err, COZIP_ERR_IO,
                       "zip_file_add(__cozip__) failed (%s)",
                       msg ? msg : "");
    }

    /* The 0xCA0C extra is attached to the LFH only. The hash starts
     * as 8 zero bytes and is patched by cozip_patch_integrity_hash.
     */
    uint8_t zero8[8] = {0};
    if (zip_file_extra_field_set(za, (zip_uint64_t)idx_id,
                                 COZIP_EXTRA_HEADER_ID, ZIP_EXTRA_FIELD_NEW,
                                 zero8, 8, ZIP_FL_LOCAL) < 0) {
        const char *msg = zip_strerror(za);
        zip_discard(za);
        return set_err(err, COZIP_ERR_IO,
                       "set 0xCA0C extra failed (%s)", msg ? msg : "");
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
            zip_discard(za);
            return set_err(err, COZIP_ERR_IO,
                           "source for '%s' failed (%s)",
                           e->arc_name, msg ? msg : "");
        }

        zip_int64_t added = zip_file_add(za, e->arc_name, src,
                                         ZIP_FL_ENC_UTF_8);
        if (added < 0) {
            const char *msg = zip_strerror(za);
            zip_source_free(src);
            zip_discard(za);
            return set_err(err, COZIP_ERR_IO,
                           "zip_file_add('%s') failed (%s)",
                           e->arc_name, msg ? msg : "");
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

    return COZIP_OK;
}


/* ---- 10. High-level finalize ---- */

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
    uint64_t end_of_payloads = (n == 0)
        ? (uint64_t)COZIP_INDEX_OFFSET + (uint64_t)index_payload_size
        : entries[n - 1].payload_offset + entries[n - 1].payload_size;

    uint64_t cd_size = (uint64_t)CD_ENTRY_BASE_SIZE
                     + (uint64_t)COZIP_INDEX_NAME_LEN;
    for (size_t i = 0; i < n; i++) {
        cd_size += (uint64_t)CD_ENTRY_BASE_SIZE
                 + (uint64_t)strlen(entries[i].arc_name);
    }
    return end_of_payloads + cd_size + (uint64_t)EOCD_SIZE;
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
    if (capacity < n_entries + 1) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "capacity (%zu) must be at least n_entries + 1 (%zu)",
                       capacity, n_entries + 1);
    }

    cozip_status_t s = cozip_plan(entries, n_entries, err);
    if (s != COZIP_OK) return s;

    size_t idx_size = 0;
    s = cozip_index_payload_size(entries, n_entries, &idx_size, err);
    if (s != COZIP_OK) return s;

    /* Padding sits last, so user entries' offsets stay put after
     * the re-plan and idx_size stays put because in_index=false.
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

        cozip_entry_t *p = &entries[n_entries];
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

    return cozip_patch_integrity_hash(out_path, idx_size, err);
}


/* ---- 11. FLAT profile ---- */

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
                           "entry %zu has NULL arc_name", i);
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

static cozip_status_t stat_file_size(const char *path, uint64_t *out_size,
                                     cozip_error_t *err) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return set_err(err, COZIP_ERR_IO, "cannot open '%s'", path);
    }
    if (cozip_fseek_end(fp) != 0) {
        fclose(fp);
        return set_err(err, COZIP_ERR_IO, "seek-end on '%s' failed", path);
    }
    long long sz = cozip_ftell64(fp);
    fclose(fp);
    if (sz < 0) {
        return set_err(err, COZIP_ERR_IO, "tell on '%s' failed", path);
    }
    *out_size = (uint64_t)sz;
    return COZIP_OK;
}

COZIP_API cozip_status_t cozip_plan_flat(cozip_entry_t *entries,
                                         size_t n_users,
                                         cozip_error_t *err) {
    if (!entries) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "entries must be non-NULL");
    }

    cozip_status_t s = reject_flat_reserved_names(entries, n_users, err);
    if (s != COZIP_OK) return s;

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
    if (capacity < n_users + 2) {
        return set_err(err, COZIP_ERR_INVALID_ARGUMENT,
                       "capacity (%zu) must be at least n_users + 2 (%zu)",
                       capacity, n_users + 2);
    }

    cozip_status_t s = reject_flat_reserved_names(entries, n_users, err);
    if (s != COZIP_OK) return s;

    uint64_t meta_size = 0;
    s = stat_file_size(metadata_path, &meta_size, err);
    if (s != COZIP_OK) return s;

    setup_flat_metadata_slot(&entries[n_users], meta_size, metadata_path);

    return cozip_finalize(out_path, entries, n_users + 1, capacity,
                          COZIP_PROFILE_FLAT, err);
}