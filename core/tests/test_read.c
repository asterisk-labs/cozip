#include "cozip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    int ok = fwrite(data, 1, size, fp) == size;
    return fclose(fp) == 0 && ok;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = (size_t)length;
    return data;
}

static uint8_t *pattern(size_t size, unsigned seed) {
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) return NULL;
    for (size_t i = 0; i < size; i++) {
        data[i] = (uint8_t)((i * 131u + seed * 17u + (i >> 7)) & 0xFFu);
    }
    return data;
}

/* FNV-1a 64 over the spec 8.2 hash input, with the declared index size. */
static uint64_t integrity_hash(const uint8_t *archive, size_t archive_size) {
    size_t index_end = COZIP_INDEX_OFFSET + read_u32(archive + 18);
    size_t suffix_start = archive_size - COZIP_HASH_WINDOW_SIZE;
    uint64_t h = COZIP_FNV_OFFSET_BASIS;
    for (size_t i = COZIP_INDEX_OFFSET; i < index_end && i < archive_size; i++) {
        h ^= archive[i];
        h *= COZIP_FNV_PRIME;
    }
    size_t resume = index_end <= suffix_start ? suffix_start : index_end;
    for (size_t i = resume; i < archive_size; i++) {
        h ^= archive[i];
        h *= COZIP_FNV_PRIME;
    }
    return h;
}

static void rehash(uint8_t *archive, size_t archive_size) {
    put_u64(archive + 43, integrity_hash(archive, archive_size));
}

/* Writes a Flat archive holding one user file and a metadata payload whose
 * bytes the reader must return unchanged. */
static int make_flat(const char *archive_path, size_t user_size,
                     size_t metadata_size, uint8_t **out_metadata) {
    char user_path[512];
    char metadata_path[512];
    snprintf(user_path, sizeof(user_path), "%s.user", archive_path);
    snprintf(metadata_path, sizeof(metadata_path), "%s.metadata", archive_path);

    uint8_t *user = pattern(user_size, 1);
    uint8_t *metadata = pattern(metadata_size, 2);
    int ok = user && metadata
          && write_bytes(user_path, user, user_size)
          && write_bytes(metadata_path, metadata, metadata_size);
    free(user);
    if (ok) {
        cozip_entry_t entries[3];
        memset(entries, 0, sizeof(entries));
        entries[0].arc_name = "data/a.bin";
        entries[0].payload_size = user_size;
        entries[0].source.kind = COZIP_SOURCE_PATH;
        entries[0].source.u.path = user_path;
        cozip_error_t err = {0};
        ok = cozip_write_flat(archive_path, entries, 1, ARRAY_LEN(entries),
                              metadata_path, &err) == COZIP_OK;
        if (!ok) fprintf(stderr, "cozip_write_flat: %s\n", err.message);
    }
    if (ok && out_metadata) {
        *out_metadata = metadata;
    } else {
        free(metadata);
    }
    return ok;
}

/* Index entry of __metadata__ in a Flat archive: offset and size. */
static void flat_metadata_range(const uint8_t *archive, uint64_t *offset,
                                uint64_t *size) {
    const uint8_t *payload = archive + COZIP_INDEX_OFFSET;
    *offset = read_u64(payload + 11 + 2 + COZIP_FLAT_METADATA_NAME_LEN);
    *size = read_u64(payload + 11 + 2 + COZIP_FLAT_METADATA_NAME_LEN + 8);
}

/* Expects status and a message containing both fragments. */
static void expect_read_error(const char *path, cozip_status_t expected,
                              const char *fragment, const char *line) {
    uint8_t *data = (uint8_t *)1;
    size_t size = 1;
    char *message = NULL;
    cozip_status_t status = cozip_read_flat_metadata(path, &data, &size,
                                                     &message);
    if (status != expected || !message || !strstr(message, fragment)
        || !strstr(message, path)) {
        fprintf(stderr, "%s: expected %s with '%s', got %s: %s\n", line,
                cozip_status_string(expected), fragment,
                cozip_status_string(status), message ? message : "(null)");
        failures++;
    }
    CHECK(data == NULL);
    CHECK(size == 0);
    cozip_free(message);
}

static void expect_patched_error(const uint8_t *archive, size_t archive_size,
                                 void (*patch)(uint8_t *, size_t), int rehashed,
                                 cozip_status_t expected, const char *fragment,
                                 const char *line) {
    const char *path = COZIP_TEST_DIR "/patched.zip";
    uint8_t *copy = (uint8_t *)malloc(archive_size);
    CHECK(copy != NULL);
    if (!copy) return;
    memcpy(copy, archive, archive_size);
    patch(copy, archive_size);
    if (rehashed) rehash(copy, archive_size);
    CHECK(write_bytes(path, copy, archive_size));
    expect_read_error(path, expected, fragment, line);
    free(copy);
}

static void test_reads_flat_metadata(void) {
    /* Small, tail and middle placements: the manifest comes from the head
     * read, from the hash window, or from its own range request. */
    static const struct {
        size_t user_size;
        size_t metadata_size;
        int where; /* 0 head, 1 tail, 2 fetched */
    } cases[] = {
        {100, 900, 0},
        {200000, 1500, 1},
        {200000, 45000, 2},
        {3, 70000, 2},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char path[256];
        snprintf(path, sizeof(path), COZIP_TEST_DIR "/flat-%zu.zip", i);
        uint8_t *expected = NULL;
        CHECK(make_flat(path, cases[i].user_size, cases[i].metadata_size,
                        &expected));
        if (!expected) continue;

        size_t archive_size = 0;
        uint8_t *archive = read_file(path, &archive_size);
        CHECK(archive != NULL);
        if (archive) {
            uint64_t offset = 0, size = 0;
            flat_metadata_range(archive, &offset, &size);
            CHECK(size == cases[i].metadata_size);
            uint64_t suffix_start = archive_size - COZIP_HASH_WINDOW_SIZE;
            if (cases[i].where == 0) CHECK(offset + size <= 65536u);
            if (cases[i].where == 1) CHECK(offset >= suffix_start);
            if (cases[i].where == 2) {
                CHECK(offset + size > 65536u);
                CHECK(offset < suffix_start);
            }
        }

        uint8_t *data = NULL;
        size_t size = 0;
        char *message = NULL;
        cozip_status_t status = cozip_read_flat_metadata(path, &data, &size,
                                                         &message);
        if (status != COZIP_OK) fprintf(stderr, "read: %s\n", message);
        CHECK(status == COZIP_OK);
        CHECK(message == NULL);
        CHECK(size == cases[i].metadata_size);
        CHECK(data && memcmp(data, expected, size) == 0);
        cozip_free(data);
        cozip_free(message);
        free(archive);
        free(expected);
    }
}

static void test_rejects_other_profiles(void) {
    /* TACO, profile 2. */
    static const uint8_t payload[] = "opaque";
    const char *data_path = COZIP_TEST_DIR "/taco-data.bin";
    const char *archive_path = COZIP_TEST_DIR "/taco.zip";
    CHECK(write_bytes(data_path, payload, sizeof(payload) - 1));
    cozip_path_entry_t files[] = {{"DATA/0/a.bin", data_path, 0, 0}};
    cozip_path_entry_t priorities[] = {
        {COZIP_TACO_COLLECTION_NAME, data_path, 0, 0},
        {"METADATA/level0.parquet", data_path, 0, 0},
    };
    cozip_taco_plan_t plan = COZIP_TACO_PLAN_INIT;
    cozip_error_t err = {0};
    CHECK(cozip_plan_taco(files, 1, priorities, 2, &plan, &err) == COZIP_OK);
    CHECK(cozip_write_taco(archive_path, files, 1, priorities, 2, &plan,
                           &err) == COZIP_OK);
    expect_read_error(archive_path, COZIP_ERR_PROFILE,
                      "read_flat needs a Flat-profile archive (profile=1). "
                      "Got profile=2 in: ", "taco");

    /* A Flat index cannot list another priority file beside __metadata__. */
    cozip_entry_t extra[3] = {0};
    extra[0].arc_name = COZIP_FLAT_METADATA_NAME;
    extra[1].arc_name = "other.bin";
    for (size_t i = 0; i < 2; i++) {
        extra[i].payload_size = sizeof(payload) - 1;
        extra[i].in_index = true;
        extra[i].source.kind = COZIP_SOURCE_BUFFER;
        extra[i].source.u.buffer.data = payload;
        extra[i].source.u.buffer.size = sizeof(payload) - 1;
    }
    const char *extra_path = COZIP_TEST_DIR "/flat-extra.zip";
    CHECK(cozip_finalize(extra_path, extra, 2, 3, COZIP_PROFILE_FLAT,
                         &err) == COZIP_OK);
    expect_read_error(extra_path, COZIP_ERR_INVALID_INDEX,
                      "Flat profile requires only '__metadata__' in the index:",
                      "flat-extra");

    /* Profile 0 with an index larger than the first 64 KiB read, so the
     * rest of the index arrives in a second request before the hash. */
    enum { N = 4000 };
    static char names[N][8];
    static uint8_t bytes[N];
    cozip_entry_t *entries = (cozip_entry_t *)calloc(N + 1, sizeof(*entries));
    CHECK(entries != NULL);
    if (!entries) return;
    for (size_t i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "e%04zu", i);
        bytes[i] = (uint8_t)i;
        entries[i].arc_name = names[i];
        entries[i].payload_size = 1;
        entries[i].in_index = true;
        entries[i].source.kind = COZIP_SOURCE_BUFFER;
        entries[i].source.u.buffer.data = &bytes[i];
        entries[i].source.u.buffer.size = 1;
    }
    const char *none_path = COZIP_TEST_DIR "/none-large-index.zip";
    cozip_status_t status = cozip_finalize(none_path, entries, N, N + 1,
                                           COZIP_PROFILE_NONE, &err);
    if (status != COZIP_OK) fprintf(stderr, "finalize: %s\n", err.message);
    CHECK(status == COZIP_OK);
    size_t archive_size = 0;
    uint8_t *archive = read_file(none_path, &archive_size);
    CHECK(archive && read_u32(archive + 18) > 65536u);
    free(archive);
    expect_read_error(none_path, COZIP_ERR_PROFILE, "Got profile=0 in: ",
                      "none");
    free(entries);
}

/* Patches over a Flat archive. Index payload layout for one entry:
 * header 51..61, name length 62, name 64..75, offset 76, size 84. */
static void patch_signature(uint8_t *a, size_t n) { (void)n; a[0] = 'X'; }
static void patch_flags(uint8_t *a, size_t n) { (void)n; a[6] |= 0x08; }
static void patch_method(uint8_t *a, size_t n) { (void)n; a[8] = 8; }
static void patch_size_pair(uint8_t *a, size_t n) { (void)n; a[22] ^= 1; }
static void patch_name_length(uint8_t *a, size_t n) { (void)n; a[26] = 8; }
static void patch_index_name(uint8_t *a, size_t n) { (void)n; a[30] = 'X'; }
static void patch_extra(uint8_t *a, size_t n) { (void)n; a[39] = 0; }
static void patch_magic(uint8_t *a, size_t n) { (void)n; a[51] = 'X'; }
static void patch_version(uint8_t *a, size_t n) { (void)n; a[55] = 2; }
static void patch_hash(uint8_t *a, size_t n) { (void)n; a[43] ^= 1; }
static void patch_tail(uint8_t *a, size_t n) { a[n - 30] ^= 1; }

static void set_payload_size(uint8_t *a, uint32_t size) {
    put_u32(a + 18, size);
    put_u32(a + 22, size);
}

static void patch_tiny_payload(uint8_t *a, size_t n) {
    (void)n;
    set_payload_size(a, 10);
}
static void patch_long_payload(uint8_t *a, size_t n) {
    set_payload_size(a, (uint32_t)n);
}
static void patch_count(uint8_t *a, size_t n) {
    (void)n;
    put_u32(a + 58, 3);
}
static void patch_no_entries(uint8_t *a, size_t n) {
    (void)n;
    put_u32(a + 58, 0);
}
static void patch_names_truncated(uint8_t *a, size_t n) {
    (void)n;
    put_u16(a + 62, 40);
}
static void patch_offsets_truncated(uint8_t *a, size_t n) {
    (void)n;
    set_payload_size(a, 30);
}
static void patch_sizes_truncated(uint8_t *a, size_t n) {
    (void)n;
    set_payload_size(a, 37);
}
static void patch_metadata_name(uint8_t *a, size_t n) {
    (void)n;
    a[75] = 'X';
}
static void patch_non_ascii(uint8_t *a, size_t n) { (void)n; a[64] = 0x80; }
static void patch_leading_slash(uint8_t *a, size_t n) { (void)n; a[64] = '/'; }
static void patch_trailing_slash(uint8_t *a, size_t n) { (void)n; a[75] = '/'; }
static void patch_drive(uint8_t *a, size_t n) { (void)n; memcpy(a + 64, "c:", 2); }
static void patch_backslash(uint8_t *a, size_t n) { (void)n; a[70] = '\\'; }
static void patch_dot(uint8_t *a, size_t n) { (void)n; memcpy(a + 64, "a/./b/", 6); }
static void patch_dotdot(uint8_t *a, size_t n) { (void)n; memcpy(a + 64, "a/../b", 6); }
static void patch_offset_outside(uint8_t *a, size_t n) { put_u64(a + 76, n); }
static void patch_offset_in_lfh(uint8_t *a, size_t n) { (void)n; put_u64(a + 76, 50); }
static void patch_size_zero(uint8_t *a, size_t n) { (void)n; put_u64(a + 84, 0); }
static void patch_size_past_end(uint8_t *a, size_t n) {
    put_u64(a + 84, n - read_u64(a + 76) + 1);
}

static void test_rejects_corrupt_archives(void) {
    const char *path = COZIP_TEST_DIR "/corrupt-source.zip";
    CHECK(make_flat(path, 100, 900, NULL));
    size_t size = 0;
    uint8_t *archive = read_file(path, &size);
    CHECK(archive != NULL);
    if (!archive) return;

    static const struct {
        void (*patch)(uint8_t *, size_t);
        int rehashed;
        cozip_status_t status;
        const char *fragment;
    } cases[] = {
        {patch_signature, 0, COZIP_ERR_INVALID_LFH, "byte 0 is not a ZIP Local File Header: "},
        {patch_flags, 0, COZIP_ERR_INVALID_LFH, "__cozip__ has a forbidden general purpose bit set: "},
        {patch_method, 0, COZIP_ERR_INVALID_LFH, "__cozip__ compression method is not STORE: "},
        {patch_size_pair, 0, COZIP_ERR_INVALID_LFH, "__cozip__ sizes are not an equal, non-zero ZIP32 pair: "},
        {patch_name_length, 0, COZIP_ERR_INVALID_LFH, "LFH does not match cozip layout: "},
        {patch_index_name, 0, COZIP_ERR_INVALID_LFH, "first ZIP entry is not __cozip__: "},
        {patch_extra, 0, COZIP_ERR_INVALID_LFH, "cozip integrity extra field (0xCA0C) missing: "},
        {patch_magic, 0, COZIP_ERR_INVALID_INDEX, "index payload magic is not 'CZIP': "},
        {patch_version, 0, COZIP_ERR_INVALID_INDEX, "unsupported cozip format version 2: "},
        {patch_tiny_payload, 0, COZIP_ERR_INVALID_INDEX, "cozip index payload is smaller than its own header: "},
        {patch_long_payload, 0, COZIP_ERR_INVALID_INDEX, "cozip index payload is truncated: "},
        {patch_hash, 0, COZIP_ERR_INVALID_INDEX, "cozip integrity hash mismatch: "},
        {patch_tail, 0, COZIP_ERR_INVALID_INDEX, "cozip integrity hash mismatch: "},
        {patch_count, 1, COZIP_ERR_INVALID_INDEX, "cozip index declares more entries than fit in its payload: "},
        {patch_no_entries, 1, COZIP_ERR_INVALID_INDEX, "cozip index sections do not match its declared size: "},
        {patch_names_truncated, 1, COZIP_ERR_INVALID_INDEX, "cozip index is truncated in its names: "},
        {patch_offsets_truncated, 1, COZIP_ERR_INVALID_INDEX, "cozip index is truncated in its offsets: "},
        {patch_sizes_truncated, 1, COZIP_ERR_INVALID_INDEX, "cozip index is truncated in its sizes: "},
        {patch_metadata_name, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_non_ascii, 1, COZIP_ERR_INVALID_INDEX, "' is not ASCII: "},
        {patch_leading_slash, 1, COZIP_ERR_INVALID_INDEX, "cozip index name '/_metadata__' starts or ends with '/': "},
        {patch_trailing_slash, 1, COZIP_ERR_INVALID_INDEX, "cozip index name '__metadata_/' starts or ends with '/': "},
        {patch_drive, 1, COZIP_ERR_INVALID_INDEX, "cozip index name 'c:metadata__' has a drive letter: "},
        {patch_backslash, 1, COZIP_ERR_INVALID_INDEX, "cozip index name '__meta\\ata__' contains a backslash: "},
        {patch_dot, 1, COZIP_ERR_INVALID_INDEX, "cozip index name 'a/./b/data__' has a '.' component: "},
        {patch_dotdot, 1, COZIP_ERR_INVALID_INDEX, "cozip index name 'a/../bdata__' has a '..' component: "},
        {patch_offset_outside, 1, COZIP_ERR_INVALID_INDEX, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_offset_in_lfh, 1, COZIP_ERR_INVALID_INDEX, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_size_zero, 1, COZIP_ERR_INVALID_INDEX, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_size_past_end, 1, COZIP_ERR_INVALID_INDEX, "cozip index entry '__metadata__' has a range outside the archive: "},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char line[32];
        snprintf(line, sizeof(line), "corrupt case %zu", i);
        expect_patched_error(archive, size, cases[i].patch, cases[i].rehashed,
                             cases[i].status, cases[i].fragment, line);
    }

    /* A prefix of a valid archive is below the minimum size. */
    const char *small_path = COZIP_TEST_DIR "/small.zip";
    CHECK(write_bytes(small_path, archive, COZIP_MIN_ARCHIVE_SIZE - 1));
    expect_read_error(small_path, COZIP_ERR_ARCHIVE_TOO_SMALL,
                      "cozip archive too small (minimum is 32819 bytes): ",
                      "small");
    free(archive);

    /* karu reports a missing local file as an I/O error naming it. */
    const char *missing = COZIP_TEST_DIR "/missing.zip";
    uint8_t *data = NULL;
    size_t data_size = 0;
    char *message = NULL;
    cozip_status_t status = cozip_read_flat_metadata(missing, &data,
                                                     &data_size, &message);
    CHECK(status == COZIP_ERR_IO || status == COZIP_ERR_NOT_FOUND);
    CHECK(message && strstr(message, missing));
    CHECK(data == NULL);
    cozip_free(message);

    /* Credentials in a URL never reach a transport message. Port 1 refuses
     * the connection at once. */
    status = cozip_read_flat_metadata(
        "http://user:secret@127.0.0.1:1/a.zip?X-Amz-Signature=abc#frag",
        &data, &data_size, &message);
    CHECK(status == COZIP_ERR_IO);
    CHECK(message && !strstr(message, "secret"));
    CHECK(message && !strstr(message, "X-Amz-Signature"));
    CHECK(message && !strstr(message, "abc"));
    CHECK(message && strstr(message, "127.0.0.1:1/a.zip"));
    cozip_free(message);
}

/* A profile 0 archive whose index lists names, read back into memory. */
static uint8_t *make_none(const char *path, const char *const *names, size_t n,
                          size_t *out_size) {
    static uint8_t byte = 7;
    cozip_entry_t entries[8];
    memset(entries, 0, sizeof(entries));
    for (size_t i = 0; i < n; i++) {
        entries[i].arc_name = names[i];
        entries[i].payload_size = 1;
        entries[i].in_index = true;
        entries[i].source.kind = COZIP_SOURCE_BUFFER;
        entries[i].source.u.buffer.data = &byte;
        entries[i].source.u.buffer.size = 1;
    }
    cozip_error_t err = {0};
    if (cozip_finalize(path, entries, n, n + 1, COZIP_PROFILE_NONE, &err)
        != COZIP_OK) {
        fprintf(stderr, "finalize: %s\n", err.message);
        return NULL;
    }
    return read_file(path, out_size);
}

/* Renames index entries in place and expects the reader's message. Names
 * start right after the 2-byte name lengths. */
static void expect_renamed(const char *const *names, size_t n,
                           const char *const *renamed, const char *fragment,
                           const char *line) {
    const char *path = COZIP_TEST_DIR "/renamed.zip";
    size_t size = 0;
    uint8_t *archive = make_none(path, names, n, &size);
    CHECK(archive != NULL);
    if (!archive) return;
    size_t cursor = COZIP_INDEX_OFFSET + COZIP_INDEX_HEADER_SIZE + 2 * n;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(names[i]);
        CHECK(memcmp(archive + cursor, names[i], len) == 0);
        CHECK(strlen(renamed[i]) == len);
        memcpy(archive + cursor, renamed[i], len);
        cursor += len;
    }
    rehash(archive, size);
    CHECK(write_bytes(path, archive, size));
    expect_read_error(path, COZIP_ERR_INVALID_INDEX, fragment, line);
    free(archive);
}

static void test_index_names(void) {
    static const char *const two[] = {"aa", "bb"};
    static const char *const repeated[] = {"aa", "aa"};
    expect_renamed(two, 2, repeated, "cozip index lists 'aa' twice: ",
                   "duplicate");

    static const char *const reserved[] = {"abcdefghi"};
    static const char *const index_name[] = {"__cozip__"};
    expect_renamed(reserved, 1, index_name,
                   "cozip index lists the reserved name '__cozip__': ",
                   "reserved index");
    static const char *const padding[] = {"abcdefghijklmnopq"};
    static const char *const padding_name[] = {"__cozip_padding__"};
    expect_renamed(padding, 1, padding_name,
                   "cozip index lists the reserved name '__cozip_padding__': ",
                   "reserved padding");

    /* The first problem in index order wins: a backslash in the second
     * entry before the third entry repeating the first. */
    static const char *const three[] = {"aa", "bb", "cc"};
    static const char *const both[] = {"aa", "b\\", "aa"};
    expect_renamed(three, 3, both,
                   "cozip index name 'b\\' contains a backslash: ", "order");
    static const char *const late[] = {"cc", "bb", "cc"};
    expect_renamed(three, 3, late, "cozip index lists 'cc' twice: ",
                   "later duplicate");
}

static void test_location_base(void) {
    static const struct {
        const char *source;
        const char *base;
    } cases[] = {
        {"/data/a.zip", "/data/a.zip"},
        {"relative/a.zip", "relative/a.zip"},
        {"~/a.zip", "~/a.zip"},
        {"file:///data/a.zip", "file:///data/a.zip"},
        {"https://example.org/a.zip", "/vsicurl/https://example.org/a.zip"},
        {"http://127.0.0.1:8000/a.zip?x=1", "/vsicurl/http://127.0.0.1:8000/a.zip?x=1"},
        {"s3://bucket/key/a.zip", "/vsis3/bucket/key/a.zip"},
        {"s3a://bucket/a.zip", "/vsis3/bucket/a.zip"},
        {"s3n://bucket/a.zip", "/vsis3/bucket/a.zip"},
        {"gs://bucket/a.zip", "/vsigs/bucket/a.zip"},
        {"gcs://bucket/a.zip", "/vsigs/bucket/a.zip"},
        {"az://container/a.zip", "/vsiaz/container/a.zip"},
        {"azure://container/a.zip", "/vsiaz/container/a.zip"},
        {"abfss://container/a.zip", "/vsiadls/container/a.zip"},
        {"hf://datasets/owner/repo/dir/a.zip",
         "/vsicurl/https://huggingface.co/datasets/owner/repo/resolve/main/dir/a.zip"},
        {"hf://datasets/owner/repo@v1.0/a.zip",
         "/vsicurl/https://huggingface.co/datasets/owner/repo/resolve/v1.0/a.zip"},
        {"hf://spaces/owner/app/a.zip",
         "/vsicurl/https://huggingface.co/spaces/owner/app/resolve/main/a.zip"},
        {"hf://owner/model@abc123/a.zip",
         "/vsicurl/https://huggingface.co/owner/model/resolve/abc123/a.zip"},
        {"hf://own@er/repo/a.zip",
         "/vsicurl/https://huggingface.co/own@er/repo/resolve/main/a.zip"},
        {"hf://datasets/asterisk-labs/cozip-api-fixtures@v0.1.0/data/cities.zip",
         "/vsicurl/https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip"},
        {"/vsis3/bucket/a.zip", "/vsis3/bucket/a.zip"},
        {"r2://bucket/a.zip", "r2://bucket/a.zip"},
        {"source://account/product/a.zip", "source://account/product/a.zip"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char *base = NULL;
        char *message = NULL;
        cozip_status_t status = cozip_location_base(cases[i].source, &base,
                                                    &message);
        CHECK(status == COZIP_OK);
        CHECK(message == NULL);
        if (!base || strcmp(base, cases[i].base) != 0) {
            fprintf(stderr, "location base of %s: got %s, want %s\n",
                    cases[i].source, base ? base : "(null)", cases[i].base);
            failures++;
        }
        cozip_free(base);
        cozip_free(message);
    }

    static const struct {
        const char *source;
        const char *message;
    } errors[] = {
        {"hf://datasets/owner", "cannot parse hf:// URL for VSI mapping"},
        {"hf://owner/repo", "cannot parse hf:// URL for VSI mapping"},
        {"hf://owner/repo@/a.zip",
         "cannot parse hf:// URL with an empty revision"},
    };
    for (size_t i = 0; i < ARRAY_LEN(errors); i++) {
        char *base = (char *)1;
        char *message = NULL;
        CHECK(cozip_location_base(errors[i].source, &base, &message)
              == COZIP_ERR_INVALID_ARGUMENT);
        CHECK(base == NULL);
        CHECK(message && strcmp(message, errors[i].message) == 0);
        cozip_free(message);
    }
}

static void test_arguments(void) {
    uint8_t *data = NULL;
    size_t size = 0;
    char *message = NULL;
    CHECK(cozip_read_flat_metadata(NULL, &data, &size, &message)
          == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(message != NULL);
    cozip_free(message);
    CHECK(cozip_read_flat_metadata("", &data, &size, &message)
          == COZIP_ERR_INVALID_ARGUMENT);
    cozip_free(message);
    CHECK(cozip_read_flat_metadata("a.zip", NULL, &size, NULL)
          == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(cozip_location_base("a.zip", NULL, NULL)
          == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strcmp(cozip_status_string(COZIP_ERR_PROFILE), "PROFILE") == 0);
    CHECK(strcmp(cozip_status_string(COZIP_ERR_NOT_FOUND), "NOT_FOUND") == 0);
    cozip_free(NULL);
}

#if !defined(_WIN32)
static void test_home_expansion(void) {
    const char *path = COZIP_TEST_DIR "/home-archive.zip";
    CHECK(make_flat(path, 10, 10, NULL));
    const char *old_home = getenv("HOME");
    char *saved = old_home ? strdup(old_home) : NULL;
    setenv("HOME", COZIP_TEST_DIR, 1);
    uint8_t *data = NULL;
    size_t size = 0;
    char *message = NULL;
    CHECK(cozip_read_flat_metadata("~/home-archive.zip", &data, &size,
                                   &message) == COZIP_OK);
    CHECK(size == 10);
    cozip_free(data);
    cozip_free(message);
    if (saved) {
        setenv("HOME", saved, 1);
        free(saved);
    } else {
        unsetenv("HOME");
    }
}
#endif

int main(void) {
    test_reads_flat_metadata();
    test_rejects_other_profiles();
    test_rejects_corrupt_archives();
    test_index_names();
    test_location_base();
    test_arguments();
#if !defined(_WIN32)
    test_home_expansion();
#endif
    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("cozip reader C API tests passed");
    return EXIT_SUCCESS;
}
