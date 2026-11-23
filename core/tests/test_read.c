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

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

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
     * read, from the hash window, from both when they meet, or from its own
     * range request. */
    static const struct {
        size_t user_size;
        size_t metadata_size;
        int where; /* 0 head, 1 tail, 2 fetched, 3 head and tail together */
    } cases[] = {
        {100, 900, 0},
        {200000, 1500, 1},
        /* The manifest starts 17 bytes into the hash window, so its local
         * header lies just before the bytes the tail holds. */
        {40000, 32560, 1},
        {200000, 45000, 2},
        {3, 70000, 3},
        /* Fetched, with a local header closer to the index than the
         * reader's look-back. */
        {3, 100000, 2},
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
                CHECK(suffix_start > 65536u);
            }
            if (cases[i].where == 3) {
                CHECK(offset + size > 65536u);
                CHECK(offset < suffix_start);
                CHECK(suffix_start <= 65536u);
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
    /* The writer refuses that layout for profile 1, so write it as profile 0
     * and mark it Flat by hand. */
    const char *extra_path = COZIP_TEST_DIR "/flat-extra.zip";
    CHECK(cozip_finalize(extra_path, extra, 2, 3, COZIP_PROFILE_FLAT,
                         &err) == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "exactly one priority file") != NULL);
    CHECK(cozip_finalize(extra_path, extra, 2, 3, COZIP_PROFILE_NONE,
                         &err) == COZIP_OK);
    size_t extra_size = 0;
    uint8_t *extra_bytes = read_file(extra_path, &extra_size);
    CHECK(extra_bytes != NULL);
    if (extra_bytes) {
        extra_bytes[COZIP_INDEX_OFFSET + 6] = COZIP_PROFILE_FLAT;
        rehash(extra_bytes, extra_size);
        CHECK(write_bytes(extra_path, extra_bytes, extra_size));
        free(extra_bytes);
    }
    expect_read_error(extra_path, COZIP_ERR_INVALID_INDEX,
                      "Flat profile requires one index entry, '__metadata__', "
                      "in 41 bytes; got 2 entries", "flat-extra");

    /* Profile 0 with an index larger than the first 64 KiB read. The
     * profile byte is in the first bytes, so the reader rejects it without
     * fetching the rest of the index. */
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
static void patch_version_zero(uint8_t *a, size_t n) { (void)n; a[55] = 0; }
static void patch_offset_in_index(uint8_t *a, size_t n) { (void)n; put_u64(a + 76, 60); }
static void patch_eocd(uint8_t *a, size_t n) { a[n - 22] = 'X'; }
static void patch_eocd_comment(uint8_t *a, size_t n) { a[n - 2] = 1; }
/* The first byte of the name in the local header of __metadata__. */
static void patch_metadata_header(uint8_t *a, size_t n) {
    (void)n;
    a[read_u64(a + 76) - COZIP_FLAT_METADATA_NAME_LEN] = 'X';
}
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
        {patch_magic, 0, COZIP_ERR_INVALID_MAGIC, "index payload magic is not 'CZIP': "},
        {patch_version, 0, COZIP_ERR_UNSUPPORTED_VERSION, "unsupported cozip format version 2: "},
        {patch_version_zero, 0, COZIP_ERR_UNSUPPORTED_VERSION, "unsupported cozip format version 0: "},
        {patch_tiny_payload, 0, COZIP_ERR_TRUNCATED_INDEX, "cozip index payload of 10 bytes does not fit the archive: "},
        {patch_long_payload, 0, COZIP_ERR_TRUNCATED_INDEX, "does not fit the archive: "},
        {patch_hash, 0, COZIP_ERR_HASH_MISMATCH, "cozip integrity hash mismatch: "},
        {patch_tail, 0, COZIP_ERR_HASH_MISMATCH, "cozip integrity hash mismatch: "},
        {patch_count, 1, COZIP_ERR_INVALID_INDEX, "Flat profile requires one index entry, '__metadata__', in 41 bytes"},
        {patch_no_entries, 1, COZIP_ERR_INVALID_INDEX, "Flat profile requires one index entry, '__metadata__', in 41 bytes"},
        {patch_names_truncated, 1, COZIP_ERR_INVALID_INDEX, "Flat profile requires one index entry, '__metadata__', in 41 bytes"},
        {patch_offsets_truncated, 1, COZIP_ERR_INVALID_INDEX, "Flat profile requires one index entry, '__metadata__', in 41 bytes"},
        {patch_sizes_truncated, 1, COZIP_ERR_INVALID_INDEX, "Flat profile requires one index entry, '__metadata__', in 41 bytes"},
        {patch_metadata_name, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_non_ascii, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_leading_slash, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_trailing_slash, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_drive, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_backslash, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_dot, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_dotdot, 1, COZIP_ERR_INVALID_INDEX, "cozip index has no entry named '__metadata__': "},
        {patch_offset_outside, 1, COZIP_ERR_INVALID_OFFSET, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_offset_in_lfh, 1, COZIP_ERR_INVALID_OFFSET, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_offset_in_index, 1, COZIP_ERR_INVALID_OFFSET, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_size_zero, 1, COZIP_ERR_INVALID_OFFSET, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_size_past_end, 1, COZIP_ERR_INVALID_OFFSET, "cozip index entry '__metadata__' has a range outside the archive: "},
        {patch_eocd, 1, COZIP_ERR_INVALID_ZIP_STRUCTURE, "comment-free End of Central Directory record: "},
        {patch_eocd_comment, 1, COZIP_ERR_INVALID_ZIP_STRUCTURE, "comment-free End of Central Directory record: "},
        {patch_metadata_header, 1, COZIP_ERR_MISSING_ENTRY, "the local header before '__metadata__' does not match the index: "},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char line[32];
        snprintf(line, sizeof(line), "corrupt case %zu", i);
        expect_patched_error(archive, size, cases[i].patch, cases[i].rehashed,
                             cases[i].status, cases[i].fragment, line);
    }

    /* The manifest header is checked wherever the manifest comes from: the
     * head and tail together, or a fetch that starts at the index end. */
    static const size_t header_cases[][2] = {{3, 70000}, {3, 100000}};
    for (size_t i = 0; i < ARRAY_LEN(header_cases); i++) {
        const char *layout_path = COZIP_TEST_DIR "/header-layout.zip";
        CHECK(make_flat(layout_path, header_cases[i][0], header_cases[i][1],
                        NULL));
        size_t layout_size = 0;
        uint8_t *layout = read_file(layout_path, &layout_size);
        CHECK(layout != NULL);
        if (!layout) continue;
        char line[32];
        snprintf(line, sizeof(line), "header layout %zu", i);
        expect_patched_error(layout, layout_size, patch_metadata_header, 1,
                             COZIP_ERR_MISSING_ENTRY,
                             "the local header before '__metadata__' does not "
                             "match the index", line);
        free(layout);
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

    /* A password holding '@' is redacted up to the last '@'. */
    status = cozip_read_flat_metadata("http://user:p@ss@127.0.0.1:1/a.zip",
                                      &data, &data_size, &message);
    CHECK(message && !strstr(message, "ss@"));
    cozip_free(message);
}

static void test_location_base(void) {
    static const struct {
        const char *source;
        const char *base;
    } cases[] = {
        {"/data/a.zip", "/data/a.zip"},
        {"relative/a.zip", "relative/a.zip"},
        {"file:///data/a.zip", "/data/a.zip"},
        {"file://localhost/data/a.zip", "/data/a.zip"},
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
        {"abfs://container/a.zip", "/vsiadls/container/a.zip"},
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
        {"source://account/product/a.zip",
         "/vsicurl/https://data.source.coop/account/product/a.zip"},
        {"hf://datasets/owner/repo/my file#1.zip",
         "/vsicurl/https://huggingface.co/datasets/owner/repo/resolve/main/my%20file%231.zip"},
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
    CHECK(strcmp(cozip_status_string(COZIP_ERR_HASH_MISMATCH),
                 "HASH_MISMATCH") == 0);
    CHECK(strcmp(cozip_status_string(COZIP_ERR_INVALID_ZIP_STRUCTURE),
                 "INVALID_ZIP_STRUCTURE") == 0);
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
    /* GDAL does not expand ~, so the location names the expanded path. */
    char *base = NULL;
    CHECK(cozip_location_base("~/home-archive.zip", &base, &message)
          == COZIP_OK);
    CHECK(base && strcmp(base, COZIP_TEST_DIR "/home-archive.zip") == 0);
    cozip_free(base);
    cozip_free(message);
    if (saved) {
        setenv("HOME", saved, 1);
        free(saved);
    } else {
        unsetenv("HOME");
    }
}
#endif

/* cozip_verify */

static void set_entry(cozip_entry_t *e, const char *name, const void *data,
                      size_t size, bool in_index) {
    memset(e, 0, sizeof(*e));
    e->arc_name = name;
    e->payload_size = size;
    e->in_index = in_index;
    e->source.kind = COZIP_SOURCE_BUFFER;
    e->source.u.buffer.data = data;
    e->source.u.buffer.size = size;
}

static int finalize_or_report(const char *path, cozip_entry_t *e, size_t n,
                              cozip_profile_t profile) {
    cozip_error_t err = {0};
    cozip_status_t status = cozip_finalize(path, e, n, n + 1, profile, &err);
    if (status != COZIP_OK) fprintf(stderr, "finalize %s: %s\n", path, err.message);
    return status == COZIP_OK;
}

/* Profile 0, three priority files among four entries. */
static int make_none(const char *path) {
    static const uint8_t a[] = "first", b[] = "second payload", c[] = "third",
                         d[] = "fourth!";
    cozip_entry_t e[5];
    set_entry(&e[0], "a.bin", a, sizeof(a) - 1, true);
    set_entry(&e[1], "dir/b.bin", b, sizeof(b) - 1, true);
    set_entry(&e[2], "c.bin", c, sizeof(c) - 1, false);
    set_entry(&e[3], "d.bin", d, sizeof(d) - 1, true);
    return finalize_or_report(path, e, 4, COZIP_PROFILE_NONE);
}

/* Where entry i of the index lives in an archive: its name, offset and size. */
typedef struct {
    size_t name, offset, size;
    uint16_t len;
} entry_pos_t;

static entry_pos_t index_entry(const uint8_t *a, uint32_t i) {
    const size_t lengths = COZIP_INDEX_OFFSET + COZIP_INDEX_HEADER_SIZE;
    const uint32_t n = read_u32(a + COZIP_INDEX_OFFSET + 7);
    size_t name = lengths + 2u * n, names_total = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t len = read_u16(a + lengths + 2u * k);
        if (k < i) name += len;
        names_total += len;
    }
    const size_t offsets = lengths + 2u * n + names_total;
    entry_pos_t pos = {name, offsets + 8u * i, offsets + 8u * n + 8u * i,
                       read_u16(a + lengths + 2u * i)};
    return pos;
}

static void expect_verify(const char *path, cozip_status_t expected,
                          const char *fragment, const char *line) {
    char *message = NULL;
    cozip_status_t status = cozip_verify(path, &message);
    int ok = status == expected &&
             (expected == COZIP_OK
                  ? message == NULL
                  : message && strstr(message, fragment) && strstr(message, path));
    if (!ok) {
        fprintf(stderr, "%s: expected %s with '%s', got %s: %s\n", line,
                cozip_status_string(expected), fragment ? fragment : "",
                cozip_status_string(status), message ? message : "(null)");
        failures++;
    }
    cozip_free(message);
}

static void verify_patched(const uint8_t *archive, size_t size,
                           void (*patch)(uint8_t *, size_t), int rehashed,
                           cozip_status_t expected, const char *fragment,
                           const char *line) {
    const char *path = COZIP_TEST_DIR "/verify-patched.zip";
    uint8_t *copy = (uint8_t *)malloc(size);
    CHECK(copy != NULL);
    if (!copy) return;
    memcpy(copy, archive, size);
    patch(copy, size);
    if (rehashed) rehash(copy, size);
    CHECK(write_bytes(path, copy, size));
    expect_verify(path, expected, fragment, line);
    free(copy);
}

static void patch_profile_reserved(uint8_t *a, size_t n) { (void)n; a[57] = 7; }
static void patch_count_plus_one(uint8_t *a, size_t n) {
    (void)n;
    put_u32(a + 58, read_u32(a + 58) + 1);
}
static void patch_count_huge(uint8_t *a, size_t n) { (void)n; put_u32(a + 58, 1000); }
static void patch_trailing_byte(uint8_t *a, size_t n) {
    (void)n;
    set_payload_size(a, read_u32(a + 18) + 1);
}
static void patch_name_absolute(uint8_t *a, size_t n) {
    (void)n;
    a[index_entry(a, 0).name] = '/';
}
static void patch_name_high_byte(uint8_t *a, size_t n) {
    (void)n;
    a[index_entry(a, 0).name + 1] = 0x80;
}
static void patch_name_dotdot(uint8_t *a, size_t n) {
    (void)n;
    memcpy(a + index_entry(a, 1).name, "dir/../bn", 9);
}
static void patch_name_reserved(uint8_t *a, size_t n) {
    (void)n;
    memcpy(a + index_entry(a, 1).name, COZIP_INDEX_NAME, 9);
}
static void patch_name_duplicate(uint8_t *a, size_t n) {
    (void)n;
    a[index_entry(a, 2).name] = 'a';
}
static void patch_entry_offset_outside(uint8_t *a, size_t n) {
    put_u64(a + index_entry(a, 0).offset, n);
}
static void patch_entry_offset_in_index(uint8_t *a, size_t n) {
    (void)n;
    put_u64(a + index_entry(a, 0).offset, 60);
}
static void patch_entry_size_zero(uint8_t *a, size_t n) {
    (void)n;
    put_u64(a + index_entry(a, 2).size, 0);
}
static void patch_last_size_zero(uint8_t *a, size_t n) {
    (void)n;
    put_u64(a + index_entry(a, read_u32(a + 58) - 1).size, 0);
}

static void test_verify(void) {
    /* Intact archives of every profile pass, and so does a reserved
     * profile value: spec 7.1 lets a reader parse an index it does not
     * recognise. */
    const char *flat = COZIP_TEST_DIR "/verify-flat.zip";
    CHECK(make_flat(flat, 100, 900, NULL));
    expect_verify(flat, COZIP_OK, NULL, "flat");

    const char *none = COZIP_TEST_DIR "/verify-none.zip";
    CHECK(make_none(none));
    expect_verify(none, COZIP_OK, NULL, "profile 0");

    static const uint8_t json[] = "{}", parquet[] = "PAR1", tile[] = "tile";
    cozip_entry_t taco_entries[4];
    set_entry(&taco_entries[0], "DATA/a.bin", tile, sizeof(tile) - 1, false);
    set_entry(&taco_entries[1], COZIP_TACO_COLLECTION_NAME, json,
              sizeof(json) - 1, true);
    set_entry(&taco_entries[2], "METADATA/level0.parquet", parquet,
              sizeof(parquet) - 1, true);
    const char *taco = COZIP_TEST_DIR "/verify-taco.zip";
    CHECK(finalize_or_report(taco, taco_entries, 3, COZIP_PROFILE_TACO));
    expect_verify(taco, COZIP_OK, NULL, "taco");

    size_t size = 0;
    uint8_t *archive = read_file(none, &size);
    CHECK(archive != NULL);
    if (!archive) return;
    verify_patched(archive, size, patch_profile_reserved, 1, COZIP_OK, NULL,
                   "reserved profile");

    /* One case per rule, in the order the checks run. */
    static const struct {
        void (*patch)(uint8_t *, size_t);
        int rehashed;
        cozip_status_t status;
        const char *fragment;
    } cases[] = {
        {patch_signature, 0, COZIP_ERR_INVALID_LFH, "byte 0 is not a ZIP Local File Header"},
        {patch_long_payload, 0, COZIP_ERR_TRUNCATED_INDEX, "does not fit the archive"},
        {patch_magic, 0, COZIP_ERR_INVALID_MAGIC, "magic is not 'CZIP'"},
        {patch_version, 0, COZIP_ERR_UNSUPPORTED_VERSION, "version 2"},
        {patch_hash, 0, COZIP_ERR_HASH_MISMATCH, "integrity hash mismatch"},
        {patch_tail, 0, COZIP_ERR_HASH_MISMATCH, "integrity hash mismatch"},
        {patch_name_absolute, 0, COZIP_ERR_HASH_MISMATCH, "integrity hash mismatch"},
        {patch_eocd_comment, 1, COZIP_ERR_INVALID_ZIP_STRUCTURE, "comment-free End of Central Directory"},
        {patch_count_huge, 1, COZIP_ERR_TRUNCATED_INDEX, "cannot hold 1000 entries"},
        {patch_count_plus_one, 1, COZIP_ERR_TRUNCATED_INDEX, "entries need"},
        {patch_trailing_byte, 1, COZIP_ERR_INVALID_INDEX, "holds only"},
        {patch_name_absolute, 1, COZIP_ERR_INVALID_NAME, "entry 0 name must be relative"},
        {patch_name_high_byte, 1, COZIP_ERR_INVALID_NAME, "entry 0 name contains non-ASCII bytes"},
        {patch_name_dotdot, 1, COZIP_ERR_INVALID_NAME, "entry 1 name contains a '.' or '..'"},
        {patch_name_reserved, 1, COZIP_ERR_INVALID_NAME, "reserved name '__cozip__'"},
        {patch_entry_offset_outside, 1, COZIP_ERR_INVALID_OFFSET, "entry 0 ('a.bin') has a range outside"},
        {patch_entry_offset_in_index, 1, COZIP_ERR_INVALID_OFFSET, "entry 0 ('a.bin') has a range outside"},
        {patch_entry_size_zero, 1, COZIP_ERR_INVALID_OFFSET, "entry 2 ('d.bin') has a range outside"},
        {patch_name_duplicate, 1, COZIP_ERR_DUPLICATE_NAME, "entries 0 and 2 share the name 'a.bin'"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char line[32];
        snprintf(line, sizeof(line), "verify case %zu", i);
        verify_patched(archive, size, cases[i].patch, cases[i].rehashed,
                       cases[i].status, cases[i].fragment, line);
    }

    const char *small = COZIP_TEST_DIR "/verify-small.zip";
    CHECK(write_bytes(small, archive, COZIP_MIN_ARCHIVE_SIZE - 1));
    expect_verify(small, COZIP_ERR_ARCHIVE_TOO_SMALL, "too small", "small");
    free(archive);

    /* An index that runs past the first 64 KiB is read whole: its sizes
     * section lies beyond the head, and a change there is hashed and
     * checked. */
    enum { MANY = 4000 };
    static char names[MANY][16];
    static const uint8_t byte = 7;
    cozip_entry_t *many = (cozip_entry_t *)calloc(MANY + 1, sizeof(*many));
    CHECK(many != NULL);
    if (many) {
        for (size_t i = 0; i < MANY; i++) {
            snprintf(names[i], sizeof(names[i]), "e%05zu.bin", i);
            set_entry(&many[i], names[i], &byte, 1, true);
        }
        const char *big = COZIP_TEST_DIR "/verify-big-index.zip";
        CHECK(finalize_or_report(big, many, MANY, COZIP_PROFILE_NONE));
        uint8_t *big_archive = read_file(big, &size);
        CHECK(big_archive != NULL);
        if (big_archive) {
            CHECK(COZIP_INDEX_OFFSET + read_u32(big_archive + 18) > 65536u);
            CHECK(index_entry(big_archive, MANY - 1).size > 65536u);
            expect_verify(big, COZIP_OK, NULL, "big index");
            verify_patched(big_archive, size, patch_last_size_zero, 0,
                           COZIP_ERR_HASH_MISMATCH, "integrity hash mismatch",
                           "big index, not rehashed");
            verify_patched(big_archive, size, patch_last_size_zero, 1,
                           COZIP_ERR_INVALID_OFFSET, "entry 3999 ('e03999.bin')",
                           "big index, rehashed");
            free(big_archive);
        }
        free(many);
    }

    /* With two repeated names, the first repeat in index order is named,
     * not the name that sorts first. */
    static const uint8_t one = 1;
    cozip_entry_t four[5];
    static const char *const four_names[] = {"w.bin", "x.bin", "y.bin", "z.bin"};
    for (size_t i = 0; i < 4; i++) set_entry(&four[i], four_names[i], &one, 1, true);
    const char *repeats = COZIP_TEST_DIR "/verify-repeats.zip";
    CHECK(finalize_or_report(repeats, four, 4, COZIP_PROFILE_NONE));
    uint8_t *repeat_archive = read_file(repeats, &size);
    CHECK(repeat_archive != NULL);
    if (repeat_archive) {
        static const char *const renamed[] = {"b.bin", "a.bin", "b.bin", "a.bin"};
        for (uint32_t i = 0; i < 4; i++) {
            memcpy(repeat_archive + index_entry(repeat_archive, i).name, renamed[i], 5);
        }
        rehash(repeat_archive, size);
        CHECK(write_bytes(repeats, repeat_archive, size));
        expect_verify(repeats, COZIP_ERR_DUPLICATE_NAME,
                      "entries 0 and 2 share the name 'b.bin'", "first repeat");
        free(repeat_archive);
    }

    /* Transport and argument errors. */
    char *message = NULL;
    cozip_status_t status = cozip_verify(COZIP_TEST_DIR "/missing.zip", &message);
    CHECK(status == COZIP_ERR_IO || status == COZIP_ERR_NOT_FOUND);
    CHECK(message && strstr(message, "missing.zip"));
    cozip_free(message);
    CHECK(cozip_verify(NULL, &message) == COZIP_ERR_INVALID_ARGUMENT);
    cozip_free(message);
    CHECK(cozip_verify("", &message) == COZIP_ERR_INVALID_ARGUMENT);
    cozip_free(message);
    CHECK(cozip_verify(none, NULL) == COZIP_OK);
    CHECK(cozip_verify(small, NULL) == COZIP_ERR_ARCHIVE_TOO_SMALL);
}

int main(void) {
    test_reads_flat_metadata();
    test_rejects_other_profiles();
    test_rejects_corrupt_archives();
    test_verify();
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
