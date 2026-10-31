/* Flat reader: verify the byte-0 index and fetch __metadata__ through karu. */

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
/* name_len (2) + offset (8) + size (8) + at least one name byte. */
#define ENTRY_MIN_SIZE   (COZIP_INDEX_PER_ENTRY_OVERHEAD + 1)
/* One read of the head holds the index of a typical archive. */
#define BOOTSTRAP_SIZE   65536u


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
    if (!scheme) return format("%s", source);
    const char *authority = scheme + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");
    const char *at = (const char *)memchr(authority, '@',
                                          (size_t)(authority_end - authority));
    const char *rest = at ? at + 1 : authority;
    size_t kept = strcspn(rest, "?#");
    const char *secret = rest[kept] == '?' ? "?<redacted>"
                       : rest[kept] == '#' ? "#<redacted>" : "";
    return format("%.*s%s%.*s%s", (int)(authority - source), source,
                  at ? "<redacted>@" : "", (int)kept, rest, secret);
}
#endif


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
        base = format("/vsicurl/https://huggingface.co/%s%.*s/resolve/%.*s/%s",
                      ns, (int)(repo_end - rest), rest,
                      (int)revision_len, revision, second + 1);
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

static bool ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

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

typedef struct index_entry {
    const char *name;     /* Points into the index bytes, not terminated. */
    uint16_t    name_len;
    uint64_t    offset;
    uint64_t    size;
    size_t      position; /* Order in the index, for duplicate detection. */
} index_entry_t;

/* cozip spec 8.5 step 1: the fixed shape of the __cozip__ local file header.
 * head holds at least the minimum archive size. */
static cozip_status_t check_prefix(const uint8_t *head, const char *source,
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
    const uint8_t *payload = head + COZIP_INDEX_OFFSET;
    if (memcmp(payload, COZIP_MAGIC, COZIP_MAGIC_LEN) != 0) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "index payload magic is not 'CZIP': %s", source);
    }
    uint16_t version = get_u16(payload + 4);
    if (version > COZIP_FORMAT_VERSION) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "unsupported cozip format version %d: %s",
                    (int)version, source);
    }
    return COZIP_OK;
}

static cozip_status_t check_name(const index_entry_t *entry,
                                 const char *source, char **out_error) {
    const char *name = entry->name;
    size_t len = entry->name_len;
    int shown = (int)len;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x01 || c > 0x7F) {
            return fail(out_error, COZIP_ERR_INVALID_INDEX,
                        "cozip index name '%.*s' is not ASCII: %s",
                        shown, name, source);
        }
    }
    if ((len == COZIP_INDEX_NAME_LEN
         && memcmp(name, COZIP_INDEX_NAME, len) == 0)
        || (len == COZIP_PADDING_NAME_LEN
            && memcmp(name, COZIP_PADDING_NAME, len) == 0)) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index lists the reserved name '%.*s': %s",
                    shown, name, source);
    }
    if (name[0] == '/' || name[len - 1] == '/') {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index name '%.*s' starts or ends with '/': %s",
                    shown, name, source);
    }
    if (len > 1 && ascii_alpha((unsigned char)name[0]) && name[1] == ':') {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index name '%.*s' has a drive letter: %s",
                    shown, name, source);
    }
    if (memchr(name, '\\', len)) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index name '%.*s' contains a backslash: %s",
                    shown, name, source);
    }
    size_t start = 0;
    while (start <= len) {
        const char *slash = (const char *)memchr(name + start, '/', len - start);
        size_t stop = slash ? (size_t)(slash - name) : len;
        size_t component = stop - start;
        if ((component == 1 && name[start] == '.')
            || (component == 2 && name[start] == '.' && name[start + 1] == '.')) {
            return fail(out_error, COZIP_ERR_INVALID_INDEX,
                        "cozip index name '%.*s' has a '%.*s' component: %s",
                        shown, name, (int)component, name + start, source);
        }
        if (!slash) break;
        start = stop + 1;
    }
    return COZIP_OK;
}

static int compare_entries(const void *a, const void *b) {
    const index_entry_t *x = *(const index_entry_t *const *)a;
    const index_entry_t *y = *(const index_entry_t *const *)b;
    size_t common = x->name_len < y->name_len ? x->name_len : y->name_len;
    int order = memcmp(x->name, y->name, common);
    if (order != 0) return order;
    if (x->name_len != y->name_len) return x->name_len < y->name_len ? -1 : 1;
    return x->position < y->position ? -1 : (x->position > y->position);
}

/* cozip spec 5.3 and 7.3 for names, then 7.5 for byte ranges. Errors come
 * in index order, as cozip_reader reports them. */
static cozip_status_t check_entries(const index_entry_t *entries, size_t n,
                                    uint64_t file_size, const char *source,
                                    char **out_error) {
    cozip_status_t status = COZIP_OK;
    const index_entry_t **sorted = NULL;
    bool *repeated = NULL;
    if (n > 0) {
        sorted = (const index_entry_t **)malloc(n * sizeof(*sorted));
        repeated = (bool *)calloc(n, sizeof(*repeated));
        if (!sorted || !repeated) {
            status = fail(out_error, COZIP_ERR_IO, "out of memory");
            goto done;
        }
    }
    for (size_t i = 0; i < n; i++) sorted[i] = &entries[i];
    if (n > 1) qsort(sorted, n, sizeof(*sorted), compare_entries);
    /* Within a run of equal names only the first in index order is new. */
    for (size_t i = 1; i < n; i++) {
        if (sorted[i]->name_len == sorted[i - 1]->name_len
            && memcmp(sorted[i]->name, sorted[i - 1]->name,
                      sorted[i]->name_len) == 0) {
            repeated[sorted[i]->position] = true;
        }
    }

    for (size_t i = 0; i < n; i++) {
        status = check_name(&entries[i], source, out_error);
        if (status != COZIP_OK) goto done;
        if (repeated[i]) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index lists '%.*s' twice: %s",
                          (int)entries[i].name_len, entries[i].name, source);
            goto done;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const index_entry_t *entry = &entries[i];
        if (entry->size == 0 || entry->offset < COZIP_INDEX_OFFSET
            || entry->offset > file_size
            || entry->size > file_size - entry->offset) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index entry '%.*s' has a range outside the archive: %s",
                          (int)entry->name_len, entry->name, source);
            goto done;
        }
    }

done:
    free(sorted);
    free(repeated);
    return status;
}

/* Parses the index sections of index_bytes, which holds the archive from
 * byte 0 to the end of the index payload. *out_entries is released with
 * free(). */
static cozip_status_t parse_entries(const uint8_t *index_bytes,
                                    uint32_t payload_size, const char *source,
                                    index_entry_t **out_entries,
                                    size_t *out_count, char **out_error) {
    const uint8_t *payload = index_bytes + COZIP_INDEX_OFFSET;
    const uint8_t *cursor = payload + COZIP_INDEX_HEADER_SIZE;
    const uint8_t *limit = payload + payload_size;

    uint32_t count = get_u32(payload + 7);
    if (count > payload_size / ENTRY_MIN_SIZE) {
        return fail(out_error, COZIP_ERR_INVALID_INDEX,
                    "cozip index declares more entries than fit in its payload: %s",
                    source);
    }
    index_entry_t *entries = NULL;
    if (count > 0) {
        entries = (index_entry_t *)calloc(count, sizeof(*entries));
        if (!entries) return fail(out_error, COZIP_ERR_IO, "out of memory");
    }

    cozip_status_t status = COZIP_OK;
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(limit - cursor) < 2) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index is truncated in its name lengths: %s",
                          source);
            goto done;
        }
        entries[i].name_len = get_u16(cursor);
        entries[i].position = i;
        if (entries[i].name_len == 0) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index has an empty name: %s", source);
            goto done;
        }
        cursor += 2;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(limit - cursor) < entries[i].name_len) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index is truncated in its names: %s", source);
            goto done;
        }
        entries[i].name = (const char *)cursor;
        cursor += entries[i].name_len;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(limit - cursor) < 8) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index is truncated in its offsets: %s", source);
            goto done;
        }
        entries[i].offset = get_u64(cursor);
        cursor += 8;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(limit - cursor) < 8) {
            status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                          "cozip index is truncated in its sizes: %s", source);
            goto done;
        }
        entries[i].size = get_u64(cursor);
        cursor += 8;
    }
    if (cursor != limit) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip index sections do not match its declared size: %s",
                      source);
        goto done;
    }

done:
    if (status != COZIP_OK) {
        free(entries);
        return status;
    }
    *out_entries = entries;
    *out_count = count;
    return COZIP_OK;
}


/* Karu transport */

/* karu names some schemes differently from DuckDB, and DuckDB expands a
 * leading ~ in local paths. Only the bytes are read through this URI;
 * cozip:location keeps the source as given. */
static char *transport_uri(const char *source) {
    if (starts_with(source, "s3a://") || starts_with(source, "s3n://")) {
        return format("s3://%s", source + 6);
    }
    if (starts_with(source, "gcs://")) return format("gs://%s", source + 6);
    if (starts_with(source, "azure://")) return format("az://%s", source + 8);
    if (starts_with(source, "abfss://")) return format("abfs://%s", source + 8);
    if (source[0] == '~' && (source[1] == '/' || source[1] == '\\')) {
        const char *home = getenv("HOME");
#if defined(_WIN32) || defined(_WIN64)
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (home && *home) return format("%s%s", home, source + 1);
    }
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

/* One open archive. The ETag pins every later read to the version whose
 * head and tail were read first. */
typedef struct archive {
    karu_client  *client;
    karu_locator *locator;
    char          etag[sizeof(((karu_object_info *)0)->etag)];
} archive_t;

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

static cozip_status_t read_metadata(const char *source, uint8_t **out_data,
                                    size_t *out_size, char **out_error) {
    cozip_status_t status = COZIP_OK;
    karu_status ks = KARU_OK;
    archive_t archive = {0};
    karu_object_info info = KARU_OBJECT_INFO_INIT;
    karu_config *config = NULL;
    char *uri = NULL;
    char *shown = NULL;
    uint8_t *head = NULL;
    uint8_t *tail = NULL;
    uint8_t *data = NULL;
    index_entry_t *entries = NULL;
    size_t n_entries = 0;
    const index_entry_t *metadata = NULL;
    uint64_t file_size = 0;
    uint64_t payload_end = 0;
    uint64_t suffix_start = 0;
    uint64_t held = 0;
    uint64_t hash = 0;
    uint32_t payload_size = 0;
    uint8_t profile = 0;

    uri = transport_uri(source);
    shown = redact_url(source);
    head = (uint8_t *)malloc(BOOTSTRAP_SIZE);
    tail = (uint8_t *)malloc(COZIP_HASH_WINDOW_SIZE);
    if (!uri || !shown || !head || !tail) {
        status = fail(out_error, COZIP_ERR_IO, "out of memory");
        goto done;
    }

    if ((ks = karu_config_create(&config)) != KARU_OK
        || (ks = karu_client_create(config, &archive.client)) != KARU_OK
        || (ks = karu_resolve(uri, &archive.locator)) != KARU_OK) {
        status = transport_error(ks, source, out_error);
        goto done;
    }

    /* The head, the size and the trailing hash window arrive together. */
    ks = karu_client_read_ends(archive.client, archive.locator,
                               head, BOOTSTRAP_SIZE,
                               tail, COZIP_HASH_WINDOW_SIZE, &info);
    if (ks != KARU_OK) {
        status = transport_error(ks, source, out_error);
        goto done;
    }
    memcpy(archive.etag, info.etag, sizeof(archive.etag));
    archive.etag[sizeof(archive.etag) - 1] = '\0';

    file_size = info.size;
    if (file_size < COZIP_MIN_ARCHIVE_SIZE) {
        status = fail(out_error, COZIP_ERR_ARCHIVE_TOO_SMALL,
                      "cozip archive too small (minimum is %llu bytes): %s",
                      (unsigned long long)COZIP_MIN_ARCHIVE_SIZE, shown);
        goto done;
    }
    status = check_prefix(head, shown, out_error);
    if (status != COZIP_OK) goto done;

    payload_size = get_u32(head + 18);
    if (payload_size < COZIP_INDEX_HEADER_SIZE) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip index payload is smaller than its own header: %s",
                      shown);
        goto done;
    }
    payload_end = (uint64_t)COZIP_INDEX_OFFSET + payload_size;
    if (payload_end > file_size) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip index payload is truncated: %s", shown);
        goto done;
    }
    held = file_size < BOOTSTRAP_SIZE ? file_size : BOOTSTRAP_SIZE;
    if (payload_end > held) {
        if (payload_end > SIZE_MAX) {
            status = fail(out_error, COZIP_ERR_IO, "out of memory");
            goto done;
        }
        uint8_t *grown = (uint8_t *)realloc(head, (size_t)payload_end);
        if (!grown) {
            status = fail(out_error, COZIP_ERR_IO, "out of memory");
            goto done;
        }
        head = grown;
        ks = read_range(&archive, held, head + held, payload_end - held);
        if (ks != KARU_OK) {
            status = transport_error(ks, source, out_error);
            goto done;
        }
        held = payload_end;
    }

    /* cozip spec 8.3: FNV-1a 64 over the index payload and the trailing
     * 32 KiB, each byte counted once where the two overlap. */
    suffix_start = file_size - COZIP_HASH_WINDOW_SIZE;
    hash = fnv1a(head + COZIP_INDEX_OFFSET, payload_size,
                 COZIP_FNV_OFFSET_BASIS);
    if (payload_end <= suffix_start) {
        hash = fnv1a(tail, COZIP_HASH_WINDOW_SIZE, hash);
    } else {
        size_t skip = (size_t)(payload_end - suffix_start);
        hash = fnv1a(tail + skip, COZIP_HASH_WINDOW_SIZE - skip, hash);
    }
    if (hash != get_u64(head + 43)) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip integrity hash mismatch: %s", shown);
        goto done;
    }

    status = parse_entries(head, payload_size, shown, &entries, &n_entries,
                           out_error);
    if (status != COZIP_OK) goto done;
    status = check_entries(entries, n_entries, file_size, shown, out_error);
    if (status != COZIP_OK) goto done;

    profile = head[COZIP_INDEX_OFFSET + 6];
    if (profile != COZIP_PROFILE_FLAT) {
        status = fail(out_error, COZIP_ERR_PROFILE,
                      "read_flat needs a Flat-profile archive (profile=1). "
                      "Got profile=%d in: %s", (int)profile, shown);
        goto done;
    }

    for (size_t i = 0; i < n_entries && !metadata; i++) {
        if (entries[i].name_len == COZIP_FLAT_METADATA_NAME_LEN
            && memcmp(entries[i].name, COZIP_FLAT_METADATA_NAME,
                      COZIP_FLAT_METADATA_NAME_LEN) == 0) {
            metadata = &entries[i];
        }
    }
    if (!metadata) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "cozip index has no entry named '%s': %s",
                      COZIP_FLAT_METADATA_NAME, shown);
        goto done;
    }
    if (n_entries != 1) {
        status = fail(out_error, COZIP_ERR_INVALID_INDEX,
                      "Flat profile requires only '__metadata__' in the index: %s",
                      shown);
        goto done;
    }
    if (metadata->size > SIZE_MAX
        || !(data = (uint8_t *)malloc((size_t)metadata->size))) {
        status = fail(out_error, COZIP_ERR_IO, "out of memory");
        goto done;
    }

    /* A manifest stored near either end is already in memory. */
    if (metadata->offset + metadata->size <= held) {
        memcpy(data, head + metadata->offset, (size_t)metadata->size);
    } else if (metadata->offset >= suffix_start) {
        memcpy(data, tail + (metadata->offset - suffix_start),
               (size_t)metadata->size);
    } else {
        ks = read_range(&archive, metadata->offset, data, metadata->size);
        if (ks != KARU_OK) {
            status = transport_error(ks, source, out_error);
            goto done;
        }
    }
    *out_data = data;
    *out_size = (size_t)metadata->size;
    data = NULL;

done:
    free(data);
    free(entries);
    free(tail);
    free(head);
    free(uri);
    free(shown);
    karu_locator_free(archive.locator);
    karu_client_free(archive.client);
    karu_config_free(config);
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
