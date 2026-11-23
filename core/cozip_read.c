/* Reader: verify the byte-0 index of any cozip, and fetch the Flat
 * __metadata__, through karu. */

#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

#include "cozip.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef COZIP_READER
#  include <karu/karu.h>
#endif

#define LFH_SIGNATURE    UINT32_C(0x04034B50)
/* Bits 0, 3, 6 and 13: encryption, data descriptor, strong encryption and
 * masked local headers. All four make the fast path unreadable. */
#define FORBIDDEN_FLAGS  0x2049u
/* One read of the head holds the whole index of a Flat archive. */
#define BOOTSTRAP_SIZE   65536u
/* A Flat index lists one name, __metadata__: 11 + 2 + 12 + 8 + 8 bytes. */
#define FLAT_INDEX_SIZE  (COZIP_INDEX_HEADER_SIZE + COZIP_INDEX_PER_ENTRY_OVERHEAD \
                          + COZIP_FLAT_METADATA_NAME_LEN)
/* The first payload after a Flat index starts after its own local header. */
#define FLAT_FIRST_PAYLOAD (COZIP_INDEX_OFFSET + FLAT_INDEX_SIZE + 30 \
                            + COZIP_FLAT_METADATA_NAME_LEN)
/* Largest extra field accepted on the local header of __metadata__. */
#define METADATA_EXTRA_MAX 1024u
#define EOCD_SIGNATURE   UINT32_C(0x06054B50)
#define EOCD_SIZE        22u


/* Errors and strings */

static char *vformat(const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0) return NULL;
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) return NULL;
    vsnprintf(text, (size_t)length + 1, fmt, ap);
    return text;
}

static char *format(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *text = vformat(fmt, ap);
    va_end(ap);
    return text;
}

/* Sets *out_error to the formatted message and returns status. */
static cozip_status_t fail(char **out_error, cozip_status_t status,
                           const char *fmt, ...) {
    if (out_error) {
        va_list ap;
        va_start(ap, fmt);
        *out_error = vformat(fmt, ap);
        va_end(ap);
    }
    return status;
}

static bool starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Keep credentials out of diagnostics; locations still use the original URL. */
#ifdef COZIP_READER
static char *redact_url(const char *source) {
    const char *scheme = strstr(source, "://");
    if (!scheme) {
        /* GDAL forms such as /vsicurl?url=... carry the URL in a query. */
        size_t kept = strcspn(source, "?");
        if (!source[kept]) return format("%s", source);
        return format("%.*s?<redacted>", (int)kept, source);
    }
    const char *authority = scheme + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");
    /* A password may itself contain '@'; userinfo ends at the last one. */
    const char *at = NULL;
    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@') at = p;
    }
    const char *rest = at ? at + 1 : authority;
    size_t kept = strcspn(rest, "?#");
    const char *secret = rest[kept] == '?' ? "?<redacted>"
                       : rest[kept] == '#' ? "#<redacted>" : "";
    return format("%.*s%s%.*s%s", (int)(authority - source), source,
                  at ? "<redacted>@" : "", (int)kept, rest, secret);
}
#endif


/* A leading ~/ expanded with HOME (USERPROFILE on Windows), or NULL when
 * source does not start with it or no home directory is known. */
static char *expand_home(const char *source) {
    if (source[0] != '~' || (source[1] != '/' && source[1] != '\\')) {
        return NULL;
    }
    const char *home = getenv("HOME");
#if defined(_WIN32) || defined(_WIN64)
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (!home || !*home) return NULL;
    return format("%s%s", home, source + 1);
}

/* Percent-encode the bytes of a URL path that GDAL and curl would misread:
 * spaces, controls, non-ASCII and the delimiters "#?%\"<>\\^`{|}. */
static char *percent_encode_path(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(path);
    if (len > (SIZE_MAX - 1) / 3) return NULL;
    char *out = (char *)malloc(len * 3 + 1);
    if (!out) return NULL;
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7F || strchr("#?%\"<>\\^`{|}", *p)) {
            *o++ = '%';
            *o++ = hex[*p >> 4];
            *o++ = hex[*p & 0x0F];
        } else {
            *o++ = (char)*p;
        }
    }
    *o = '\0';
    return out;
}


/* GDAL location base */

/* Match cozip_reader, with GDAL mappings for az://, s3a:// and s3n://. */
static cozip_status_t vsi_base(const char *path, char **out,
                               char **out_error) {
    static const struct {
        const char *scheme;
        const char *vsi;
    } prefixes[] = {
        {"s3://",     "/vsis3/"},
        {"s3a://",    "/vsis3/"},
        {"s3n://",    "/vsis3/"},
        {"gcs://",    "/vsigs/"},
        {"gs://",     "/vsigs/"},
        {"abfss://",  "/vsiadls/"},
        {"abfs://",   "/vsiadls/"},
        {"azure://",  "/vsiaz/"},
        {"az://",     "/vsiaz/"},
    };

    char *base = NULL;
    if (starts_with(path, "https://") || starts_with(path, "http://")) {
        base = format("/vsicurl/%s", path);
    } else if (starts_with(path, "hf://")) {
        const char *rest = path + 5;
        const char *ns = "";
        if (starts_with(rest, "datasets/")) {
            ns = "datasets/";
            rest += 9;
        } else if (starts_with(rest, "spaces/")) {
            ns = "spaces/";
            rest += 7;
        }
        const char *first = strchr(rest, '/');
        const char *second = first ? strchr(first + 1, '/') : NULL;
        if (!second) {
            return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                        "cannot parse hf:// URL for VSI mapping");
        }
        const char *repo_end = second;
        const char *revision = "main";
        size_t revision_len = 4;
        const char *at = (const char *)memchr(first + 1, '@',
                                              (size_t)(second - first - 1));
        if (at) {
            repo_end = at;
            revision = at + 1;
            revision_len = (size_t)(second - revision);
            if (revision_len == 0) {
                return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                            "cannot parse hf:// URL with an empty revision");
            }
        }
        /* karu percent-encodes the file path; the location must name the
         * same URL, so a space or a '#' in a file name survives curl. */
        char *encoded = percent_encode_path(second + 1);
        if (!encoded) return fail(out_error, COZIP_ERR_IO, "out of memory");
        base = format("/vsicurl/https://huggingface.co/%s%.*s/resolve/%.*s/%s",
                      ns, (int)(repo_end - rest), rest,
                      (int)revision_len, revision, encoded);
        free(encoded);
    } else if (starts_with(path, "source://")) {
        /* Source Cooperative serves the same objects over HTTPS, at the
         * endpoint karu reads them from by default. */
        base = format("/vsicurl/https://data.source.coop/%s", path + 9);
    } else if (starts_with(path, "file://")) {
        const char *local = path + 7;
        if (starts_with(local, "localhost/")) local += 9;
        base = format("%s", local);
    } else if ((base = expand_home(path)) != NULL) {
        /* the bytes are read from the expanded path; GDAL does not expand ~ */
    } else {
        const char *vsi = "";
        const char *rest = path;
        for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            if (starts_with(path, prefixes[i].scheme)) {
                vsi = prefixes[i].vsi;
                rest = path + strlen(prefixes[i].scheme);
                break;
            }
        }
        base = format("%s%s", vsi, rest);
    }
    if (!base) return fail(out_error, COZIP_ERR_IO, "out of memory");
    *out = base;
    return COZIP_OK;
}


/* Index verification */

#ifdef COZIP_READER

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t hash) {
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)data[i];
        hash *= COZIP_FNV_PRIME;
    }
    return hash;
}

/* cozip spec 8.5 step 1: the fixed shape of the 51-byte __cozip__ local file
 * header. head holds at least 51 bytes. */
static cozip_status_t check_lfh(const uint8_t *head, const char *source,
                                char **out_error) {
    if (get_u32(head) != LFH_SIGNATURE) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "byte 0 is not a ZIP Local File Header: %s", source);
    }
    if (get_u16(head + 6) & FORBIDDEN_FLAGS) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "__cozip__ has a forbidden general purpose bit set: %s",
                    source);
    }
    if (get_u16(head + 8) != 0) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "__cozip__ compression method is not STORE: %s", source);
    }
    uint32_t compressed = get_u32(head + 18);
    uint32_t uncompressed = get_u32(head + 22);
    if (compressed != uncompressed || compressed == 0
        || compressed == UINT32_C(0xFFFFFFFF)) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "__cozip__ sizes are not an equal, non-zero ZIP32 pair: %s",
                    source);
    }
    if (get_u16(head + 26) != COZIP_INDEX_NAME_LEN
        || get_u16(head + 28) != COZIP_EXTRA_FIELD_SIZE) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "LFH does not match cozip layout: %s", source);
    }
    if (memcmp(head + 30, COZIP_INDEX_NAME, COZIP_INDEX_NAME_LEN) != 0) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "first ZIP entry is not __cozip__: %s", source);
    }
    if (get_u16(head + 39) != COZIP_EXTRA_HEADER_ID
        || get_u16(head + 41) != COZIP_EXTRA_DATA_SIZE) {
        return fail(out_error, COZIP_ERR_INVALID_LFH,
                    "cozip integrity extra field (0xCA0C) missing: %s", source);
    }
    return COZIP_OK;
}

/* Whether buf, which holds archive bytes [buf_start, buf_start + buf_len),
 * contains the local file header of __metadata__ ending exactly at offset,
 * with sizes that match size (spec 9, MISSING_ENTRY). Extra fields of up to
 * METADATA_EXTRA_MAX bytes are allowed, as other writers may add them.
 * No header starts before floor, the end of the index. HEADER_UNSEEN means
 * the header would start before the bytes in hand but after floor. */
typedef enum { HEADER_MATCH, HEADER_MISMATCH, HEADER_UNSEEN } header_check_t;

static header_check_t metadata_header(const uint8_t *buf, uint64_t buf_start,
                                      uint64_t buf_len, uint64_t floor,
                                      uint64_t offset, uint64_t size) {
    const uint64_t fixed = 30 + COZIP_FLAT_METADATA_NAME_LEN;
    for (uint64_t extra = 0; extra <= METADATA_EXTRA_MAX; extra++) {
        if (offset < floor + fixed + extra) return HEADER_MISMATCH;
        if (offset < buf_start + fixed + extra) return HEADER_UNSEEN;
        uint64_t at = offset - fixed - extra - buf_start;
        if (at + fixed + extra > buf_len) continue;
        const uint8_t *h = buf + at;
        if (get_u32(h) != LFH_SIGNATURE ||
            get_u16(h + 26) != COZIP_FLAT_METADATA_NAME_LEN ||
            get_u16(h + 28) != extra ||
            memcmp(h + 30, COZIP_FLAT_METADATA_NAME,
                   COZIP_FLAT_METADATA_NAME_LEN) != 0) {
            continue;
        }
        if ((get_u16(h + 6) & FORBIDDEN_FLAGS) || get_u16(h + 8) != 0) {
            return HEADER_MISMATCH;
        }
        uint32_t c = get_u32(h + 18);
        uint32_t u = get_u32(h + 22);
        if (c != UINT32_C(0xFFFFFFFF) || u != UINT32_C(0xFFFFFFFF)) {
            return (c == u && (uint64_t)c == size) ? HEADER_MATCH
                                                   : HEADER_MISMATCH;
        }
        /* ZIP64: the 0x0001 extra carries both sizes. */
        const uint8_t *x = h + fixed;
        const uint8_t *x_end = x + extra;
        while (x + 4 <= x_end) {
            uint16_t id = get_u16(x);
            uint16_t len = get_u16(x + 2);
            if (x + 4 + len > x_end) return HEADER_MISMATCH;
            if (id == 0x0001u && len >= 16) {
                return (get_u64(x + 4) == size && get_u64(x + 12) == size)
                     ? HEADER_MATCH : HEADER_MISMATCH;
            }
            x += 4 + len;
        }
        return HEADER_MISMATCH;
    }
    return HEADER_MISMATCH;
}


/* Karu transport */

/* karu names some schemes differently from DuckDB, and DuckDB expands a
 * leading ~ in local paths, as the location base does too. */
static char *transport_uri(const char *source) {
    if (starts_with(source, "s3a://") || starts_with(source, "s3n://")) {
        return format("s3://%s", source + 6);
    }
    if (starts_with(source, "gcs://")) return format("gs://%s", source + 6);
    if (starts_with(source, "azure://")) return format("az://%s", source + 8);
    if (starts_with(source, "abfss://")) return format("abfs://%s", source + 8);
    char *home = expand_home(source);
    if (home) return home;
    return format("%s", source);
}

static cozip_status_t transport_error(karu_status status, const char *source,
                                      char **out_error) {
    const char *detail = karu_last_error();
    if (!detail || !*detail) detail = karu_status_string(status);
    cozip_status_t code = COZIP_ERR_IO;
    if (status == KARU_ERR_NOT_FOUND) {
        code = COZIP_ERR_NOT_FOUND;
    } else if (status == KARU_ERR_URI || status == KARU_ERR_INVALID) {
        code = COZIP_ERR_INVALID_ARGUMENT;
    } else if (status == KARU_ERR_UNSUPPORTED) {
        code = COZIP_ERR_UNSUPPORTED;
    }
    char *shown = redact_url(source);
    if (!shown) return fail(out_error, code, "%s", karu_status_string(status));
    if (strcmp(shown, source) != 0) {
        fail(out_error, code, "could not read %s: %s", shown,
             karu_status_string(status));
        free(shown);
        return code;
    }
    if (strstr(detail, source) || strstr(detail, shown)) {
        free(shown);
        return fail(out_error, code, "%s", detail);
    }
    fail(out_error, code, "could not read %s: %s", shown, detail);
    free(shown);
    return code;
}

/* One open archive: the first answer, its head and tail, and the ETag that
 * pins every later read to the version they came from. */
typedef struct archive {
    karu_config  *config;
    karu_client  *client;
    karu_locator *locator;
    char          etag[sizeof(((karu_object_info *)0)->etag)];
    char         *shown;  /* the source without credentials, for messages */
    uint64_t      size;
    uint8_t      *head;   /* archive bytes [0, held) */
    uint64_t      held;
    uint8_t      *tail;   /* the last 32 KiB, once size is checked */
} archive_t;

static void close_archive(archive_t *archive) {
    free(archive->head);
    free(archive->tail);
    free(archive->shown);
    karu_locator_free(archive->locator);
    karu_client_free(archive->client);
    karu_config_free(archive->config);
}

static karu_status read_range(const archive_t *archive, uint64_t offset,
                              uint8_t *buffer, uint64_t length) {
    karu_req request = {
        .locator = archive->locator,
        .offset = offset,
        .length = length,
        .buffer = buffer,
        .tag = NULL,
        .if_match = archive->etag[0] ? archive->etag : NULL,
    };
    return karu_client_fetch(archive->client, &request, 1);
}

/* Resolves source and requests the head, the size and the trailing hash
 * window at once; later reads are pinned to the strong ETag of this answer.
 * close_archive releases archive whatever this returns. */
static cozip_status_t open_archive(const char *source, archive_t *archive,
                                   char **out_error) {
    char *uri = transport_uri(source);
    archive->shown = redact_url(source);
    archive->head = (uint8_t *)malloc(BOOTSTRAP_SIZE);
    archive->tail = (uint8_t *)malloc(COZIP_HASH_WINDOW_SIZE);
    if (!uri || !archive->shown || !archive->head || !archive->tail) {
        free(uri);
        return fail(out_error, COZIP_ERR_IO, "out of memory");
    }
    karu_status ks;
    if ((ks = karu_config_create(&archive->config)) != KARU_OK
        || (ks = karu_client_create(archive->config, &archive->client)) != KARU_OK
        || (ks = karu_resolve(uri, &archive->locator)) != KARU_OK) {
        free(uri);
        return transport_error(ks, source, out_error);
    }
    free(uri);

    karu_object_info info = KARU_OBJECT_INFO_INIT;
    ks = karu_client_read_ends(archive->client, archive->locator,
                               archive->head, BOOTSTRAP_SIZE,
                               archive->tail, COZIP_HASH_WINDOW_SIZE, &info);
    if (ks != KARU_OK) return transport_error(ks, source, out_error);
    memcpy(archive->etag, info.etag, sizeof(archive->etag));
    archive->etag[sizeof(archive->etag) - 1] = '\0';
    archive->size = info.size;
    archive->held = info.size < BOOTSTRAP_SIZE ? info.size : BOOTSTRAP_SIZE;
    return COZIP_OK;
}

/* Spec 8.5 in its order, from the head alone: the local header (step 1),
 * the archive size (step 5), then the index header the reader must check
 * before anything else (7.1). Sets *out_payload_size, the index size. */
static cozip_status_t check_head(const archive_t *archive,
                                 uint32_t *out_payload_size,
                                 char **out_error) {
    const char *shown = archive->shown;
    /* A file too short to hold the header is too small. */
    if (archive->size < COZIP_INDEX_OFFSET) {
        return fail(out_error, COZIP_ERR_ARCHIVE_TOO_SMALL,
                    "cozip archive too small (minimum is %llu bytes): %s",
                    (unsigned long long)COZIP_MIN_ARCHIVE_SIZE, shown);
    }
    cozip_status_t status = check_lfh(archive->head, shown, out_error);
    if (status != COZIP_OK) return status;
    const uint32_t payload_size = get_u32(archive->head + 18);
    if (payload_size < COZIP_INDEX_HEADER_SIZE ||
        (uint64_t)COZIP_INDEX_OFFSET + payload_size > archive->size) {
        return fail(out_error, COZIP_ERR_TRUNCATED_INDEX,
                    "cozip index payload of %u bytes does not fit the "
                    "archive: %s", (unsigned)payload_size, shown);
    }
    if (archive->size < COZIP_MIN_ARCHIVE_SIZE) {
        return fail(out_error, COZIP_ERR_ARCHIVE_TOO_SMALL,
                    "cozip archive too small (minimum is %llu bytes): %s",
                    (unsigned long long)COZIP_MIN_ARCHIVE_SIZE, shown);
    }

    /* The archive is at least 32,819 bytes, so the head holds the 11-byte
     * index header. */
    const uint8_t *index = archive->head + COZIP_INDEX_OFFSET;
    if (memcmp(index, COZIP_MAGIC, COZIP_MAGIC_LEN) != 0) {
        return fail(out_error, COZIP_ERR_INVALID_MAGIC,
                    "index payload magic is not 'CZIP': %s", shown);
    }
    const uint16_t version = get_u16(index + 4);
    if (version == 0 || version > COZIP_FORMAT_VERSION) {
        return fail(out_error, COZIP_ERR_UNSUPPORTED_VERSION,
                    "unsupported cozip format version %d: %s",
                    (int)version, shown);
    }
    *out_payload_size = payload_size;
    return COZIP_OK;
}

/* Spec 8.2 and 8.3: FNV-1a 64 over the index payload and the trailing
 * 32 KiB, each byte counted once where the two overlap (steps 6 to 8), then
 * 5.1.11: the comment-free end record is the last 22 bytes. */
static cozip_status_t check_hash_and_end(const archive_t *archive,
                                         const uint8_t *index,
                                         uint32_t payload_size,
                                         char **out_error) {
    const uint64_t payload_end = (uint64_t)COZIP_INDEX_OFFSET + payload_size;
    const uint64_t suffix_start = archive->size - COZIP_HASH_WINDOW_SIZE;
    uint64_t hash = fnv1a(index, payload_size, COZIP_FNV_OFFSET_BASIS);
    if (payload_end <= suffix_start) {
        hash = fnv1a(archive->tail, COZIP_HASH_WINDOW_SIZE, hash);
    } else {
        size_t skip = (size_t)(payload_end - suffix_start);
        hash = fnv1a(archive->tail + skip, COZIP_HASH_WINDOW_SIZE - skip, hash);
    }
    if (hash != get_u64(archive->head + 43)) {
        return fail(out_error, COZIP_ERR_HASH_MISMATCH,
                    "cozip integrity hash mismatch: %s", archive->shown);
    }
    const uint8_t *eocd = archive->tail + COZIP_HASH_WINDOW_SIZE - EOCD_SIZE;
    if (get_u32(eocd) != EOCD_SIGNATURE || get_u16(eocd + 20) != 0) {
        return fail(out_error, COZIP_ERR_INVALID_ZIP_STRUCTURE,
                    "archive does not end with a comment-free End of "
                    "Central Directory record: %s", archive->shown);
    }
    return COZIP_OK;
}

static cozip_status_t read_metadata(const char *source, uint8_t **out_data,
                                    size_t *out_size, char **out_error) {
    archive_t archive = {0};
    uint8_t *data = NULL;
    uint8_t *fetched = NULL;
    uint32_t payload_size = 0;

    cozip_status_t status = open_archive(source, &archive, out_error);
    if (status != COZIP_OK) goto done;
    status = check_head(&archive, &payload_size, out_error);
    if (status != COZIP_OK) goto done;

    const char *shown = archive.shown;
    const uint64_t file_size = archive.size;
    const uint64_t held = archive.held;
    const uint8_t *head = archive.head;
    const uint8_t *tail = archive.tail;
    const uint8_t *index = head + COZIP_INDEX_OFFSET;

    /* A Flat index has one shape, so the profile, the count and the size
     * are checked from the bytes in hand before anything else is trusted,
     * and an archive of another profile costs no further request. */
    const uint8_t profile = index[6];
    if (profile != COZIP_PROFILE_FLAT) {
        status = fail(out_error, COZIP_ERR_PROFILE,
                      "read_flat needs a Flat-profile archive (profile=1). "
                      "Got profile=%d in: %s", (int)profile, shown);
        goto done;
    }
    if (get_u32(index + 7) != 1 || payload_size != FLAT_INDEX_SIZE ||
        get_u16(index + 11) != COZIP_FLAT_METADATA_NAME_LEN) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "Flat profile requires one index entry, '%s', in %u "
                      "bytes; got %u entries in %u bytes: %s",
                      COZIP_FLAT_METADATA_NAME, (unsigned)FLAT_INDEX_SIZE,
                      (unsigned)get_u32(index + 7), (unsigned)payload_size,
                      shown);
        goto done;
    }

    status = check_hash_and_end(&archive, index, payload_size, out_error);
    if (status != COZIP_OK) goto done;

    const uint64_t payload_end = (uint64_t)COZIP_INDEX_OFFSET + payload_size;
    const uint64_t suffix_start = file_size - COZIP_HASH_WINDOW_SIZE;
    const uint8_t *name = index + 13;
    if (memcmp(name, COZIP_FLAT_METADATA_NAME,
               COZIP_FLAT_METADATA_NAME_LEN) != 0) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip index has no entry named '%s': %s",
                      COZIP_FLAT_METADATA_NAME, shown);
        goto done;
    }
    const uint64_t offset = get_u64(name + COZIP_FLAT_METADATA_NAME_LEN);
    const uint64_t size = get_u64(name + COZIP_FLAT_METADATA_NAME_LEN + 8);
    /* Spec 7.5: a non-empty range inside the archive, after the index and
     * the manifest's own local header. */
    if (size == 0 || offset < FLAT_FIRST_PAYLOAD || offset > file_size ||
        size > file_size - offset) {
        status = fail(out_error, COZIP_ERR_INVALID_OFFSET,
                      "cozip index entry '%s' has a range outside the "
                      "archive: %s", COZIP_FLAT_METADATA_NAME, shown);
        goto done;
    }
    if (size > SIZE_MAX || !(data = (uint8_t *)malloc((size_t)size))) {
        status = fail(out_error, COZIP_ERR_IO,
                      "out of memory for a %llu-byte manifest: %s",
                      (unsigned long long)size, shown);
        goto done;
    }

    /* The manifest usually lies in bytes already in hand, from the same
     * answer the hash was checked on. When the head and the tail meet,
     * their union holds the whole archive. */
    const uint64_t end = offset + size;
    bool copied = false;
    if (end <= held) {
        memcpy(data, head + offset, (size_t)size);
        copied = true;
    } else if (offset >= suffix_start) {
        memcpy(data, tail + (offset - suffix_start), (size_t)size);
        copied = true;
    } else if (suffix_start <= held) {
        size_t first = (size_t)(held - offset);
        memcpy(data, head + offset, first);
        memcpy(data + first, tail + (held - suffix_start),
               (size_t)size - first);
        copied = true;
    }

    if (copied) {
        /* Check the manifest's local header when the same buffer holds it
         * (spec 9 MISSING_ENTRY); a header that starts before the buffer
         * cannot be checked and is not held against the archive. A manifest
         * that starts inside the head has its header there. */
        header_check_t header = HEADER_UNSEEN;
        if (offset <= held) {
            header = metadata_header(head, 0, held, payload_end, offset, size);
        } else if (offset >= suffix_start) {
            header = metadata_header(tail, suffix_start,
                                     COZIP_HASH_WINDOW_SIZE, payload_end,
                                     offset, size);
        }
        if (header == HEADER_MISMATCH) {
            status = fail(out_error, COZIP_ERR_MISSING_ENTRY,
                          "the local header before '%s' does not match the "
                          "index: %s", COZIP_FLAT_METADATA_NAME, shown);
            goto done;
        }
    } else {
        /* One more range, from a little before the manifest so its local
         * header comes along. The header check catches an object replaced
         * since the first answer when no strong ETag pins the read. */
        /* offset is past the index, so offset - payload_end cannot wrap. */
        uint64_t lookback = 30 + COZIP_FLAT_METADATA_NAME_LEN + METADATA_EXTRA_MAX;
        uint64_t from = offset - payload_end > lookback ? offset - lookback
                                                        : payload_end;
        uint64_t length = end - from;
        if (length > SIZE_MAX ||
            !(fetched = (uint8_t *)malloc((size_t)length))) {
            status = fail(out_error, COZIP_ERR_IO,
                          "out of memory for a %llu-byte manifest: %s",
                          (unsigned long long)size, shown);
            goto done;
        }
        karu_status ks = read_range(&archive, from, fetched, length);
        if (ks != KARU_OK) {
            status = transport_error(ks, source, out_error);
            goto done;
        }
        /* The range starts far enough back for any header this reader
         * accepts, or at the end of the index, so an unseen header is as
         * wrong as a mismatched one. */
        if (metadata_header(fetched, from, length, payload_end, offset, size)
            != HEADER_MATCH) {
            status = fail(out_error, COZIP_ERR_MISSING_ENTRY,
                          "the local header before '%s' does not match the "
                          "index; the archive may have changed while it was "
                          "read: %s", COZIP_FLAT_METADATA_NAME, shown);
            goto done;
        }
        memcpy(data, fetched + (offset - from), (size_t)size);
    }
    *out_data = data;
    *out_size = (size_t)size;
    data = NULL;

done:
    free(fetched);
    free(data);
    close_archive(&archive);
    return status;
}

/* Index verification for any profile */

const char *cozip_internal_name_problem(const char *name, size_t len);

/* One indexed name, for the duplicate check. */
typedef struct index_name {
    const uint8_t *name;
    uint16_t       len;
    uint32_t       entry;
} index_name_t;

static int compare_index_names(const void *left, const void *right) {
    const index_name_t *a = (const index_name_t *)left;
    const index_name_t *b = (const index_name_t *)right;
    size_t shorter = a->len < b->len ? a->len : b->len;
    int order = memcmp(a->name, b->name, shorter);
    if (order != 0) return order;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    return a->entry < b->entry ? -1 : (a->entry > b->entry);
}

/* At most this many bytes of a name go into a message. */
#define SHOWN_NAME_MAX 120

/* Spec 7.2 to 7.5 and 5.3: the five sections fill the payload exactly, every
 * name is a valid archive name other than __cozip__ and __cozip_padding__,
 * no name repeats, and every range is non-empty and lies inside the archive
 * after the index and the entry's own local header. */
static cozip_status_t check_index(const archive_t *archive,
                                  const uint8_t *index, uint32_t payload_size,
                                  char **out_error) {
    const char *shown = archive->shown;
    const uint32_t count = get_u32(index + 7);
    uint64_t needed = (uint64_t)COZIP_INDEX_HEADER_SIZE
                    + (uint64_t)COZIP_INDEX_PER_ENTRY_OVERHEAD * count;
    if (needed > payload_size) {
        return fail(out_error, COZIP_ERR_TRUNCATED_INDEX,
                    "cozip index payload of %u bytes cannot hold %u "
                    "entries: %s", (unsigned)payload_size, (unsigned)count,
                    shown);
    }
    const uint8_t *lengths = index + COZIP_INDEX_HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        needed += get_u16(lengths + 2u * (size_t)i);
    }
    if (needed > payload_size) {
        return fail(out_error, COZIP_ERR_TRUNCATED_INDEX,
                    "cozip index payload of %u bytes is shorter than its %u "
                    "entries need (%llu bytes): %s", (unsigned)payload_size,
                    (unsigned)count, (unsigned long long)needed, shown);
    }
    if (needed < payload_size) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index payload of %u bytes holds only %llu bytes "
                    "of sections: %s", (unsigned)payload_size,
                    (unsigned long long)needed, shown);
    }

    /* The sections fill the payload exactly, so the offsets and the sizes
     * are its last 16 bytes per entry. */
    const uint8_t *names = lengths + 2u * (size_t)count;
    const uint8_t *offsets = index + payload_size - 16u * (size_t)count;
    const uint8_t *sizes = offsets + 8u * (size_t)count;
    const uint64_t payload_end = (uint64_t)COZIP_INDEX_OFFSET + payload_size;

    index_name_t *sorted = NULL;
    if (count > 0) {
        sorted = (index_name_t *)malloc((size_t)count * sizeof(*sorted));
        if (!sorted) return fail(out_error, COZIP_ERR_IO, "out of memory");
    }
    cozip_status_t status = COZIP_OK;
    const uint8_t *name = names;
    for (uint32_t i = 0; i < count; i++) {
        const uint16_t len = get_u16(lengths + 2u * (size_t)i);
        const char *problem =
            cozip_internal_name_problem((const char *)name, len);
        if (problem) {
            status = fail(out_error, COZIP_ERR_INVALID_NAME,
                          "cozip index entry %u name %s: %s", (unsigned)i,
                          problem, shown);
            goto done;
        }
        const int shown_len = len > SHOWN_NAME_MAX ? SHOWN_NAME_MAX : (int)len;
        if ((len == COZIP_INDEX_NAME_LEN &&
             memcmp(name, COZIP_INDEX_NAME, len) == 0) ||
            (len == COZIP_PADDING_NAME_LEN &&
             memcmp(name, COZIP_PADDING_NAME, len) == 0)) {
            status = fail(out_error, COZIP_ERR_INVALID_NAME,
                          "cozip index entry %u uses the reserved name "
                          "'%.*s', which is never indexed: %s", (unsigned)i,
                          shown_len, (const char *)name, shown);
            goto done;
        }
        const uint64_t offset = get_u64(offsets + 8u * (size_t)i);
        const uint64_t size = get_u64(sizes + 8u * (size_t)i);
        if (size == 0 || offset < payload_end + 30u + len ||
            offset > archive->size || size > archive->size - offset) {
            status = fail(out_error, COZIP_ERR_INVALID_OFFSET,
                          "cozip index entry %u ('%.*s') has a range outside "
                          "the archive: %s", (unsigned)i, shown_len,
                          (const char *)name, shown);
            goto done;
        }
        sorted[i].name = name;
        sorted[i].len = len;
        sorted[i].entry = i;
        name += len;
    }
    /* Names and ranges come first; then the first name that repeats in
     * index order, reported with the entry it repeats, as the JavaScript
     * reader does. Sorting by name and entry puts each name's first entry
     * at the start of its run. */
    if (count > 1) {
        qsort(sorted, count, sizeof(*sorted), compare_index_names);
        const index_name_t *first = NULL;
        const index_name_t *repeat = NULL;
        const index_name_t *run = &sorted[0];
        for (uint32_t i = 1; i < count; i++) {
            const index_name_t *b = &sorted[i];
            if (b->len != run->len || memcmp(b->name, run->name, b->len) != 0) {
                run = b;
                continue;
            }
            if (!repeat || b->entry < repeat->entry) {
                first = run;
                repeat = b;
            }
        }
        if (repeat) {
            const int shown_len =
                first->len > SHOWN_NAME_MAX ? SHOWN_NAME_MAX : (int)first->len;
            status = fail(out_error, COZIP_ERR_DUPLICATE_NAME,
                          "cozip index entries %u and %u share the name "
                          "'%.*s': %s", (unsigned)first->entry,
                          (unsigned)repeat->entry, shown_len,
                          (const char *)first->name, shown);
            goto done;
        }
    }

done:
    free(sorted);
    return status;
}

static cozip_status_t verify_archive(const char *source, char **out_error) {
    archive_t archive = {0};
    uint8_t *whole = NULL;
    uint32_t payload_size = 0;

    cozip_status_t status = open_archive(source, &archive, out_error);
    if (status != COZIP_OK) goto done;
    status = check_head(&archive, &payload_size, out_error);
    if (status != COZIP_OK) goto done;

    /* Spec 8.5 step 3 reads the whole index. Only an index of another
     * profile than Flat can run past the head, and then one more range,
     * pinned like the rest, reads what is missing. */
    const uint8_t *index = archive.head + COZIP_INDEX_OFFSET;
    const uint64_t payload_end = (uint64_t)COZIP_INDEX_OFFSET + payload_size;
    if (payload_end > archive.held) {
        const size_t in_head = (size_t)(archive.held - COZIP_INDEX_OFFSET);
        if (!(whole = (uint8_t *)malloc(payload_size))) {
            status = fail(out_error, COZIP_ERR_IO,
                          "out of memory for a %u-byte index: %s",
                          (unsigned)payload_size, archive.shown);
            goto done;
        }
        memcpy(whole, index, in_head);
        karu_status ks = read_range(&archive, archive.held, whole + in_head,
                                    payload_end - archive.held);
        if (ks != KARU_OK) {
            status = transport_error(ks, source, out_error);
            goto done;
        }
        index = whole;
    }

    status = check_hash_and_end(&archive, index, payload_size, out_error);
    if (status != COZIP_OK) goto done;
    status = check_index(&archive, index, payload_size, out_error);

done:
    free(whole);
    close_archive(&archive);
    return status;
}

#endif


/* Public API */

COZIP_API cozip_status_t cozip_read_flat_metadata(const char *source,
                                                  uint8_t **out_data,
                                                  size_t *out_size,
                                                  char **out_error) {
    if (out_error) *out_error = NULL;
    if (!source || !out_data || !out_size) {
        return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                    "cozip_read_flat_metadata: source and outputs must not be NULL");
    }
    *out_data = NULL;
    *out_size = 0;
    if (!*source) {
        return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                    "cozip_read_flat_metadata: source is empty");
    }
#ifdef COZIP_READER
    return read_metadata(source, out_data, out_size, out_error);
#else
    return fail(out_error, COZIP_ERR_UNSUPPORTED,
                "this libcozip was built without the Flat reader");
#endif
}

COZIP_API cozip_status_t cozip_verify(const char *source, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!source) {
        return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                    "cozip_verify: source must not be NULL");
    }
    if (!*source) {
        return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                    "cozip_verify: source is empty");
    }
#ifdef COZIP_READER
    return verify_archive(source, out_error);
#else
    return fail(out_error, COZIP_ERR_UNSUPPORTED,
                "this libcozip was built without the reader");
#endif
}

COZIP_API cozip_status_t cozip_location_base(const char *source,
                                             char **out_base,
                                             char **out_error) {
    if (out_error) *out_error = NULL;
    if (!source || !out_base) {
        return fail(out_error, COZIP_ERR_INVALID_ARGUMENT,
                    "cozip_location_base: source and output must not be NULL");
    }
    *out_base = NULL;
    return vsi_base(source, out_base, out_error);
}

COZIP_API void cozip_free(void *ptr) {
    free(ptr);
}
