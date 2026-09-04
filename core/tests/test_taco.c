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
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
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

static int write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    int ok = fwrite(data, 1, size, fp) == size;
    return fclose(fp) == 0 && ok;
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
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

/* FNV-1a 64 over the spec 8.2 hash input: index region, then the
 * trailing 32 KiB with any overlap hashed once. */
static uint64_t expected_integrity_hash(const uint8_t *archive,
                                        size_t archive_size) {
    uint32_t index_size = read_u32(archive + 18);
    size_t index_end = COZIP_INDEX_OFFSET + index_size;
    size_t suffix_start = archive_size - COZIP_HASH_WINDOW_SIZE;
    uint64_t h = COZIP_FNV_OFFSET_BASIS;
    for (size_t i = COZIP_INDEX_OFFSET; i < index_end; i++) {
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

static size_t collect_local_names(const uint8_t *archive,
                                  size_t archive_size,
                                  char names[][64],
                                  size_t payload_offsets[],
                                  size_t payload_sizes[],
                                  size_t names_capacity) {
    size_t cursor = 0;
    size_t count = 0;
    while (cursor + 30 <= archive_size &&
           read_u32(archive + cursor) == UINT32_C(0x04034B50)) {
        uint32_t payload_size = read_u32(archive + cursor + 18);
        uint16_t name_size = read_u16(archive + cursor + 26);
        uint16_t extra_size = read_u16(archive + cursor + 28);
        if (cursor + 30u + name_size + extra_size > archive_size) return 0;
        size_t payload_offset = cursor + 30u + name_size + extra_size;
        if ((size_t)payload_size > archive_size - payload_offset) return 0;
        if (count >= names_capacity || name_size >= 64u) return 0;
        memcpy(names[count], archive + cursor + 30u, name_size);
        names[count][name_size] = '\0';
        payload_offsets[count] = payload_offset;
        payload_sizes[count] = payload_size;
        count++;
        cursor = payload_offset + payload_size;
    }
    return count;
}

static void test_taco_plan_and_write(void) {
    static const uint8_t image_bytes[] = "image-opaque";
    static const uint8_t mask_bytes[] = "mask-opaque-bytes";
    static const uint8_t collection_bytes[] = "not-json: cozip-must-not-care";
    static const uint8_t metadata_bytes[] = "not-parquet-either";
    const char *image_path = COZIP_TEST_DIR "/image.bin";
    const char *mask_path = COZIP_TEST_DIR "/mask.bin";
    const char *collection_path = COZIP_TEST_DIR "/collection.bin";
    const char *metadata_path = COZIP_TEST_DIR "/metadata.bin";
    const char *archive_path = COZIP_TEST_DIR "/dataset.zip";
    const char *drift_path = COZIP_TEST_DIR "/drift.zip";

    remove(archive_path);
    remove(drift_path);
    CHECK(write_bytes(image_path, image_bytes, sizeof(image_bytes) - 1));
    CHECK(write_bytes(mask_path, mask_bytes, sizeof(mask_bytes) - 1));
    CHECK(write_bytes(collection_path, collection_bytes,
                      sizeof(collection_bytes) - 1));
    CHECK(write_bytes(metadata_path, metadata_bytes,
                      sizeof(metadata_bytes) - 1));

    cozip_path_entry_t files[] = {
        {"DATA/0/image.tif", image_path, 0, 0},
        {"DATA/0/mask.tif", mask_path, 0, 0},
    };
    cozip_path_entry_t planned_priorities[] = {
        {COZIP_TACO_COLLECTION_NAME, NULL, 0, 0},
        {"METADATA/sample.parquet", NULL, 0, 0},
    };
    cozip_taco_plan_t plan = COZIP_TACO_PLAN_INIT;
    cozip_error_t err = {0};
    cozip_status_t status = cozip_plan_taco(
        files, ARRAY_LEN(files), planned_priorities,
        ARRAY_LEN(planned_priorities), &plan, &err);
    CHECK(status == COZIP_OK);
    CHECK(plan.struct_size == sizeof(plan));
    CHECK(plan.abi_version == COZIP_TACO_PLAN_VERSION);
    CHECK(plan.n_files == ARRAY_LEN(files));
    CHECK(plan.n_priorities == ARRAY_LEN(planned_priorities));
    CHECK(files[0].payload_size == sizeof(image_bytes) - 1);
    CHECK(files[1].payload_size == sizeof(mask_bytes) - 1);
    CHECK(files[0].payload_offset < files[1].payload_offset);

    cozip_path_entry_t priorities[] = {
        {COZIP_TACO_COLLECTION_NAME, collection_path, 0, 0},
        {"METADATA/sample.parquet", metadata_path, 0, 0},
    };
    status = cozip_write_taco(
        archive_path, files, ARRAY_LEN(files), priorities,
        ARRAY_LEN(priorities), &plan, &err);
    if (status != COZIP_OK) {
        fprintf(stderr, "cozip_write_taco: %s\n", err.message);
    }
    CHECK(status == COZIP_OK);

    size_t archive_size = 0;
    uint8_t *archive = read_file(archive_path, &archive_size);
    CHECK(archive != NULL);
    if (archive) {
        CHECK(archive_size >= COZIP_MIN_ARCHIVE_SIZE);
        CHECK(read_u32(archive) == UINT32_C(0x04034B50));
        CHECK(archive[COZIP_INDEX_OFFSET + 6] == COZIP_PROFILE_TACO);
        CHECK(read_u32(archive + COZIP_INDEX_OFFSET + 7) == 2u);
        CHECK(files[0].payload_offset + sizeof(image_bytes) - 1 <= archive_size);
        CHECK(memcmp(archive + files[0].payload_offset, image_bytes,
                     sizeof(image_bytes) - 1) == 0);
        CHECK(files[1].payload_offset + sizeof(mask_bytes) - 1 <= archive_size);
        CHECK(memcmp(archive + files[1].payload_offset, mask_bytes,
                     sizeof(mask_bytes) - 1) == 0);

        /* Integrity hash patched and correct (spec 8). */
        CHECK(read_u64(archive + 43) != 0);
        CHECK(read_u64(archive + 43) ==
              expected_integrity_hash(archive, archive_size));

        /* Comment-free EOCD is the final record (spec 5.1.11). */
        CHECK(archive_size >= 22);
        CHECK(read_u32(archive + archive_size - 22) == UINT32_C(0x06054B50));
        CHECK(read_u16(archive + archive_size - 2) == 0);

        char names[8][64] = {{0}};
        size_t payload_offsets[8] = {0};
        size_t payload_sizes[8] = {0};
        size_t count = collect_local_names(archive, archive_size,
                                           names, payload_offsets,
                                           payload_sizes, ARRAY_LEN(names));
        CHECK(count == 6);
        if (count == 6) {
            CHECK(strcmp(names[0], COZIP_INDEX_NAME) == 0);
            CHECK(strcmp(names[1], "DATA/0/image.tif") == 0);
            CHECK(strcmp(names[2], "DATA/0/mask.tif") == 0);
            CHECK(strcmp(names[3], COZIP_PADDING_NAME) == 0);
            CHECK(strcmp(names[4], COZIP_TACO_COLLECTION_NAME) == 0);
            CHECK(strcmp(names[5], "METADATA/sample.parquet") == 0);
            CHECK(payload_sizes[4] == sizeof(collection_bytes) - 1);
            CHECK(memcmp(archive + payload_offsets[4], collection_bytes,
                         sizeof(collection_bytes) - 1) == 0);
            CHECK(payload_sizes[5] == sizeof(metadata_bytes) - 1);
            CHECK(memcmp(archive + payload_offsets[5], metadata_bytes,
                         sizeof(metadata_bytes) - 1) == 0);
        }
        free(archive);
    }

    /* Output must never replace one of its own sources. */
    status = cozip_write_taco(
        image_path, files, ARRAY_LEN(files), priorities,
        ARRAY_LEN(priorities), &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    size_t preserved_size = 0;
    uint8_t *preserved = read_file(image_path, &preserved_size);
    CHECK(preserved != NULL);
    if (preserved) {
        CHECK(preserved_size == sizeof(image_bytes) - 1);
        CHECK(memcmp(preserved, image_bytes, sizeof(image_bytes) - 1) == 0);
        free(preserved);
    }

    /* Source size drift between plan and write is rejected before the
     * output is opened. */
    static const uint8_t changed_image[] = "different-size";
    CHECK(write_bytes(image_path, changed_image, sizeof(changed_image) - 1));
    status = cozip_write_taco(
        drift_path, files, ARRAY_LEN(files), priorities,
        ARRAY_LEN(priorities), &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(!file_exists(drift_path));

    remove(image_path);
    remove(mask_path);
    remove(collection_path);
    remove(metadata_path);
    remove(archive_path);
    remove(drift_path);
}

static void test_taco_validation(void) {
    static const uint8_t byte = 1;
    const char *source_path = COZIP_TEST_DIR "/validation.bin";
    CHECK(write_bytes(source_path, &byte, 1));

    cozip_error_t err = {0};
    cozip_taco_plan_t plan = COZIP_TACO_PLAN_INIT;
    cozip_path_entry_t collection = {COZIP_TACO_COLLECTION_NAME, NULL, 0, 0};

    cozip_taco_plan_t missing_init = {0};
    cozip_path_entry_t ordinary = {"DATA/0/a.bin", source_path, 0, 0};
    cozip_status_t status = cozip_plan_taco(
        &ordinary, 1, &collection, 1, &missing_init, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "plan struct") != NULL);

    /* Archive-wide duplicate across the two partitions. */
    cozip_path_entry_t duplicate = {COZIP_TACO_COLLECTION_NAME, source_path, 0, 0};
    status = cozip_plan_taco(
        &duplicate, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "duplicate") != NULL);

    char non_ascii_bytes[] = {(char)0xC0, (char)0xAF, '\0'};
    cozip_path_entry_t invalid = {non_ascii_bytes, source_path, 0, 0};
    status = cozip_plan_taco(&invalid, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);

    cozip_path_entry_t empty_name = {"", source_path, 0, 0};
    status = cozip_plan_taco(&empty_name, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);

    /* Spec 14.2: COLLECTION.json must be a priority file. */
    cozip_path_entry_t data = {"DATA/0/a.bin", source_path, 0, 0};
    cozip_path_entry_t parquet_only = {"METADATA/sample.parquet", NULL, 0, 0};
    status = cozip_plan_taco(&data, 1, &parquet_only, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, COZIP_TACO_COLLECTION_NAME) != NULL);

    /* Spec 14.2: METADATA Parquet files cannot be non-priority. */
    cozip_path_entry_t stray_parquet = {"METADATA/stray.parquet", source_path, 0, 0};
    status = cozip_plan_taco(&stray_parquet, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "14.2") != NULL);

    cozip_path_entry_t dot_parquet = {"METADATA/.parquet", source_path, 0, 0};
    status = cozip_plan_taco(&dot_parquet, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "14.2") != NULL);

    /* Archive names are ASCII-only. Local paths are not archive names. */
    cozip_path_entry_t non_ascii_name = {
        "DATA/0/ni\xC3\xB1o.bin", source_path, 0, 0
    };
    status = cozip_plan_taco(&non_ascii_name, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "non-ASCII") != NULL);

#ifndef _WIN32
    const char *non_ascii_path = COZIP_TEST_DIR "/ni\xC3\xB1o.bin";
    CHECK(write_bytes(non_ascii_path, &byte, 1));
    cozip_path_entry_t unicode_source = {
        "DATA/0/file.bin", non_ascii_path, 0, 0
    };
    status = cozip_plan_taco(&unicode_source, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_OK);
    CHECK(unicode_source.payload_size == 1);
    remove(non_ascii_path);
#endif

    /* A METADATA file that is not Parquet, or a Parquet outside
     * METADATA/, may stay non-priority. */
    cozip_path_entry_t allowed[] = {
        {"METADATA/notes.txt", source_path, 0, 0},
        {"DATA/0/table.parquet", source_path, 0, 0},
    };
    status = cozip_plan_taco(allowed, ARRAY_LEN(allowed), &collection, 1,
                             &plan, &err);
    CHECK(status == COZIP_OK);

    /* Sources must be regular files. */
    cozip_path_entry_t directory = {"DATA/0/dir", COZIP_TEST_DIR, 0, 0};
    status = cozip_plan_taco(&directory, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "regular file") != NULL);

    cozip_path_entry_t missing = {"DATA/0/missing", COZIP_TEST_DIR "/nope", 0, 0};
    status = cozip_plan_taco(&missing, 1, &collection, 1, &plan, &err);
    CHECK(status == COZIP_ERR_IO);

    /* Low-level guards. */
    cozip_entry_t indexed = {0};
    indexed.arc_name = "indexed";
    indexed.in_index = true;
    size_t payload_size = 0;
    status = cozip_index_payload_size(&indexed, 1, &payload_size, &err);
    CHECK(status == COZIP_OK);
    status = cozip_build_index_payload(
        &indexed, 1, COZIP_PROFILE_NONE, NULL, payload_size, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);

    indexed.arc_name = COZIP_PADDING_NAME;
    status = cozip_index_payload_size(&indexed, 1, &payload_size, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "must not appear") != NULL);

    indexed.arc_name = "indexed";
    uint8_t tiny_index[COZIP_INDEX_HEADER_SIZE + 7 +
                       COZIP_INDEX_PER_ENTRY_OVERHEAD];
    status = cozip_build_index_payload(
        &indexed, 1, (cozip_profile_t)256, tiny_index,
        sizeof(tiny_index), &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "one byte") != NULL);

    /* Exactly 0xFFFFFFFF bytes is the one size libzip cannot encode. */
    cozip_entry_t boundary = {0};
    boundary.arc_name = "boundary";
    boundary.payload_size = UINT64_C(0xFFFFFFFF);
    status = cozip_plan(&boundary, 1, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    boundary.payload_size = UINT64_C(0x100000000);
    status = cozip_plan(&boundary, 1, &err);
    CHECK(status == COZIP_OK);
    CHECK(boundary.lfh_size == 30 + 8 + 20);

    /* The C planner owns archive-wide uniqueness and the byte-zero name. */
    cozip_entry_t duplicate_entries[2] = {0};
    duplicate_entries[0].arc_name = "same.bin";
    duplicate_entries[1].arc_name = "same.bin";
    status = cozip_plan(duplicate_entries, 2, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "duplicate archive name") != NULL);

    cozip_entry_t reserved_entry = {0};
    reserved_entry.arc_name = COZIP_INDEX_NAME;
    status = cozip_plan(&reserved_entry, 1, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "reserved name") != NULL);

    /* FLAT user files are real payloads, not planning placeholders. */
    cozip_entry_t flat_entries[2] = {0};
    flat_entries[0].arc_name = "empty.bin";
    status = cozip_plan_flat(flat_entries, 1, &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "zero-byte payload") != NULL);

    flat_entries[0].payload_size = 1;
    flat_entries[0].in_index = true;
    status = cozip_plan_flat(flat_entries, 1, &err);
    CHECK(status == COZIP_OK);
    CHECK(flat_entries[0].in_index == false);
    CHECK(flat_entries[1].in_index == true);

    remove(source_path);
}

/* cozip_write_archive must compare the archive it wrote against the
 * plan it was given; an unplanned entry (lfh_offset left at 0) cannot
 * match and the bogus file must not survive. */
static void test_write_archive_post_write_check(void) {
    static const uint8_t payload[] = "unplanned";
    const char *archive_path = COZIP_TEST_DIR "/unplanned.zip";
    const char *source_path = COZIP_TEST_DIR "/path-source.bin";
    remove(archive_path);
    remove(source_path);

    cozip_entry_t entry = {0};
    entry.arc_name = "data.bin";
    entry.payload_size = sizeof(payload) - 1;
    entry.in_index = false;
    entry.source.kind = COZIP_SOURCE_BUFFER;
    entry.source.u.buffer.data = payload;
    entry.source.u.buffer.size = sizeof(payload) - 1;

    cozip_error_t err = {0};
    uint8_t index_payload[COZIP_INDEX_HEADER_SIZE];
    cozip_status_t status = cozip_build_index_payload(
        &entry, 1, COZIP_PROFILE_NONE, index_payload,
        sizeof(index_payload), &err);
    CHECK(status == COZIP_OK);

    status = cozip_write_archive(archive_path, &entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    CHECK(status == COZIP_ERR_INVALID_LFH);
    CHECK(!file_exists(archive_path));

    /* A zero-byte entry is valid as a planning placeholder but cannot be
     * written into a conforming cozip archive. */
    entry.payload_size = 0;
    entry.source.u.buffer.size = 0;
    status = cozip_plan(&entry, 1, &err);
    CHECK(status == COZIP_OK);
    status = cozip_write_archive(archive_path, &entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "zero-byte payload") != NULL);
    CHECK(!file_exists(archive_path));

    entry.payload_size = sizeof(payload) - 1;
    entry.source.u.buffer.size = sizeof(payload) - 1;

    /* Once planned, the same entry round-trips. */
    status = cozip_plan(&entry, 1, &err);
    CHECK(status == COZIP_OK);
    status = cozip_write_archive(archive_path, &entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    if (status != COZIP_OK) {
        fprintf(stderr, "cozip_write_archive: %s\n", err.message);
    }
    CHECK(status == COZIP_OK);
    CHECK(file_exists(archive_path));

    /* A buffer whose size disagrees with payload_size is refused before
     * the output is touched. */
    remove(archive_path);
    entry.source.u.buffer.size = 1;
    status = cozip_write_archive(archive_path, &entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(!file_exists(archive_path));

    /* Path sources are re-statted before the output is opened. */
    CHECK(write_bytes(source_path, payload, sizeof(payload) - 1));
    cozip_entry_t path_entry = {0};
    path_entry.arc_name = "path.bin";
    path_entry.payload_size = sizeof(payload) - 2;
    path_entry.source.kind = COZIP_SOURCE_PATH;
    path_entry.source.u.path = source_path;
    status = cozip_plan(&path_entry, 1, &err);
    CHECK(status == COZIP_OK);
    status = cozip_write_archive(archive_path, &path_entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "planned payload size") != NULL);
    CHECK(!file_exists(archive_path));

    /* The generic writer cannot replace a path source either. */
    path_entry.payload_size = sizeof(payload) - 1;
    status = cozip_plan(&path_entry, 1, &err);
    CHECK(status == COZIP_OK);
    status = cozip_write_archive(source_path, &path_entry, 1, index_payload,
                                 sizeof(index_payload), &err);
    CHECK(status == COZIP_ERR_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "also the source path") != NULL);
    size_t source_size = 0;
    uint8_t *source = read_file(source_path, &source_size);
    CHECK(source != NULL);
    if (source) {
        CHECK(source_size == sizeof(payload) - 1);
        CHECK(memcmp(source, payload, sizeof(payload) - 1) == 0);
        free(source);
    }

    remove(archive_path);
    remove(source_path);
}

int main(void) {
    test_taco_plan_and_write();
    test_taco_validation();
    test_write_archive_post_write_check();
    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("cozip TACO C API tests passed");
    return EXIT_SUCCESS;
}
