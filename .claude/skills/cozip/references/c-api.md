# C API

The public header is `core/cozip.h`; the implementation is the single file
`core/cozip.c`, linked statically against the vendored libzip 1.11.4 and zlib 1.3.1. The
examples below compiled with `-std=c11 -Wall -Wextra` and ran against a 2026.9.9 build on
macOS arm64. The C library only writes; there is no C reader API.

## Contents

1. Build and link
2. Conventions and status codes
3. The API in layers
4. `cozip_entry_t`, capacity and what gets mutated
5. Example: one call with `cozip_finalize`
6. Example: a Flat archive in two stages
7. What is validated, and failure semantics
8. Constants
9. Notes for FFI bindings

## 1. Build and link

```bash
make lib                      # CMake + Ninja into core/build/, copies the library to python/cozip/_lib/

cmake -S core -B core/build-tests -G Ninja -DCOZIP_BUILD_TESTS=ON   # core/build-*/ is ignored by git
cmake --build core/build-tests
ctest --test-dir core/build-tests --output-on-failure
```

- Options: `COZIP_BUILD_TESTS` (default `OFF`), `COZIP_INSTALL` (default `ON`, forced off
  under Emscripten). The build type defaults to `Release` and the macOS deployment target
  to 11.0. The version comes from the repository's `VERSION` file and lands in the
  generated `<build>/version.h`.
- The shared library has no `lib` prefix: `cozip.dylib`, `cozip.so`, `cozip.dll`. `-lcozip`
  therefore fails (`ld: library 'cozip' not found`); link the file itself and set an rpath:

  ```bash
  cc -std=c11 app.c -Icore -Icore/build core/build/cozip.dylib -Wl,-rpath,$PWD/core/build -o app
  ```

- Inside a CMake project, `add_subdirectory(core)` and link `cozip::cozip`. An installed
  tree exports the same target through `cozipConfig.cmake`.
- GitHub releases carry `libcozip-<version>-<platform>.tar.gz` (plus a `.zip` for Windows)
  with `include/cozip.h`, `include/version.h`, and `lib/cozip.{so,dylib}` or
  `bin/cozip.dll`. Platforms: `linux-x86_64`, `linux-aarch64` (manylinux 2.28),
  `macos-universal`, `windows-x86_64`.
- Only `cozip_*` symbols are exported (`COZIP_API`, hidden visibility); the release build
  fails if a libzip or zlib symbol leaks.

## 2. Conventions and status codes

- Functions return `cozip_status_t`, except the string accessors,
  `cozip_build_extra_field` (void) and the two `uint64_t` size predictors.
- `cozip_error_t *err` may be `NULL`. When given, `err->code` and a NUL-terminated message
  of at most 192 bytes (`COZIP_ERROR_MESSAGE_SIZE`) are filled on failure. Output
  parameters are unspecified after a failure.
- Archive names are NUL-terminated ASCII. Filesystem paths are UTF-8; the Windows build
  converts them to UTF-16. The caller owns every string and buffer, which must outlive the
  call.

| Status | Value | Meaning |
| --- | --- | --- |
| `COZIP_OK` | 0 | success |
| `COZIP_ERR_INVALID_LFH` | 1 | the written archive did not match the plan (post-write check); the output was removed |
| `COZIP_ERR_ARCHIVE_TOO_SMALL` | 2 | hash patch on a file below 32,819 bytes |
| `COZIP_ERR_INVALID_ARGUMENT` | 100 | names, sizes, sources, capacity, profile rules, plan drift |
| `COZIP_ERR_BUFFER_TOO_SMALL` | 101 | index output buffer too small |
| `COZIP_ERR_IO` | 102 | open, stat, read, write, libzip, allocation or cleanup failure |

`cozip_status_string(status)` returns the name (`"INVALID_ARGUMENT"`).

## 3. The API in layers

| Layer | Functions | Use when |
| --- | --- | --- |
| One call | `cozip_finalize` | every payload exists; profile 0, or you enforce a profile yourself |
| Flat profile | `cozip_plan_flat`, `cozip_write_flat` | Flat archives: plan, write the Parquet manifest with the offsets, then write |
| TACO profile | `cozip_plan_taco`, `cozip_write_taco` | TACO writers (`taco-profile.md`) |
| Primitives | `cozip_plan`, `cozip_index_payload_size`, `cozip_build_index_payload`, `cozip_build_extra_field`, `cozip_write_archive`, `cozip_patch_integrity_hash`, `cozip_predict_zip32_archive_size`, `cozip_required_padding_payload` | a custom pipeline; `cozip_finalize` is the reference composition |
| Accessors | `cozip_version_string`, `cozip_status_string`, `cozip_index_name`, `cozip_padding_name`, `cozip_flat_metadata_name` | bindings that avoid macros |

`cozip_finalize` runs: optional TACO block check, `cozip_plan`, size prediction and
padding, `cozip_build_index_payload`, `cozip_write_archive` (libzip write plus post-write
verification), `cozip_patch_integrity_hash`. With `COZIP_PROFILE_FLAT` it records the
profile byte but checks no names: an index that lists files other than `__metadata__`
is written without complaint and is not a conforming Flat archive. Use
`cozip_write_flat` for Flat.

## 4. `cozip_entry_t`, capacity and what gets mutated

```c
typedef struct cozip_entry {
    const char     *arc_name;        /* input: ASCII archive name */
    uint64_t        payload_size;    /* input: must match the file or buffer */
    bool            in_index;        /* input: list in the byte-0 index */
    cozip_source_t  source;          /* input: COZIP_SOURCE_PATH or COZIP_SOURCE_BUFFER */
    uint64_t lfh_offset;             /* output of cozip_plan */
    uint64_t lfh_size;               /* output: 30 + name, plus 20 above 0xFFFFFFFF bytes */
    uint64_t payload_offset;         /* output: lfh_offset + lfh_size */
} cozip_entry_t;
```

| Call | Array length |
| --- | --- |
| `cozip_finalize(out, entries, n, capacity, profile, err)` | `capacity >= n + 1` |
| `cozip_plan_flat(entries, n_users, err)` | at least `n_users + 1` |
| `cozip_write_flat(out, entries, n_users, capacity, metadata_path, err)` | `capacity >= n_users + 2` |

The functions reuse the array as scratch space:

- `cozip_plan` fills the three output fields of every entry.
- The Flat helpers set `in_index = false` on user entries and turn `entries[n_users]` into
  the `__metadata__` slot (size 0 and no source while planning).
- When padding is needed, `cozip_finalize` writes a `__cozip_padding__` entry into the
  spare slot. For profile 2 it first shifts the priority block one slot to the right so
  the padding precedes it. The padding buffer is freed before the call returns, so that
  slot's `source.u.buffer.data` dangles afterwards.
- A `COZIP_SOURCE_BUFFER` entry needs `buffer.size == payload_size`; a `COZIP_SOURCE_PATH`
  entry is re-stat'ed and must still have `payload_size` bytes.

## 5. Example: one call with `cozip_finalize`

```c
#include "cozip.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    const char *names[] = {"notes/a.txt", "notes/b.txt"};
    const char *paths[] = {"a.txt", "b.txt"};
    cozip_entry_t entries[3] = {0};   /* n + 1: room for padding */

    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "wb");
        if (!f) return 1;
        for (int j = 0; j < 100; j++) fprintf(f, "file %d line %d\n", i, j);
        long size = ftell(f);
        fclose(f);

        entries[i].arc_name      = names[i];
        entries[i].payload_size  = (uint64_t)size;
        entries[i].in_index      = true;       /* profile 0: index both files */
        entries[i].source.kind   = COZIP_SOURCE_PATH;
        entries[i].source.u.path = paths[i];
    }

    cozip_error_t err = {0};
    cozip_status_t s = cozip_finalize("dataset.zip", entries, 2, 3,
                                      COZIP_PROFILE_NONE, &err);
    if (s != COZIP_OK) {
        fprintf(stderr, "%s: %s\n", cozip_status_string(s), err.message);
        return 1;
    }
    for (int i = 0; i < 2; i++)
        printf("%s offset=%llu size=%llu\n", entries[i].arc_name,
               (unsigned long long)entries[i].payload_offset,
               (unsigned long long)entries[i].payload_size);
    return 0;
}
```

Output: `notes/a.txt offset=161 size=1490`, `notes/b.txt offset=1692 size=1490`; the
archive is padded to 32,819 bytes and `entries[2]` became `__cozip_padding__`.

## 6. Example: a Flat archive in two stages

```c
/* Stage 1: plan. entries holds n_users files plus two spare slots. */
cozip_entry_t entries[N_USERS + 2] = {0};
/* ... fill arc_name, payload_size and a COZIP_SOURCE_PATH source for each user file ... */
cozip_error_t err = {0};
if (cozip_plan_flat(entries, N_USERS, &err) != COZIP_OK) { /* report err.message */ }
/* entries[i].payload_offset and payload_size are final: write a Parquet file with
   columns name (string), offset (uint64), size (uint64) and any extras, one row per
   user file, using any Parquet library. */

/* Stage 2: write, embedding that Parquet as __metadata__. */
cozip_status_t s = cozip_write_flat("dataset.zip", entries, N_USERS, N_USERS + 2,
                                    "metadata.parquet", &err);
```

Verified end to end: C planned offsets 133 and 1664, Python wrote the Parquet from them,
C wrote the archive, and `cozip.read` returned both rows with an extra column. The user
offsets do not depend on the manifest size because `__metadata__` is placed after every
user entry and the Flat index is always 41 bytes.

## 7. What is validated, and failure semantics

Before the output is opened:

- Names: syntax, ASCII, length 1 to 65,535, archive-wide uniqueness, `__cozip__`
  reserved, `__cozip_padding__` never indexed; the Flat helpers also reserve
  `__metadata__`, and the TACO helpers add the rules in `taco-profile.md`.
- Sizes: no zero-byte payload in a written archive (zero is allowed only as a planning
  placeholder), no payload of exactly `0xFFFFFFFF` bytes, no u64 overflow, at most
  `UINT32_MAX` indexed entries, index payload below 4 GiB.
- Sources: path sources must be regular files whose current size equals `payload_size`;
  buffers must match their size; `COZIP_SOURCE_NONE` is refused.
- The output must not be one of the sources, compared by spelling and, when the output
  exists, by file identity (device and inode on POSIX, volume and file index on Windows).

While writing:

- libzip writes to a temporary file and renames it over the output only when `zip_close`
  succeeds, so earlier failures leave an existing file untouched.
- The archive is then re-read: the 51-byte `__cozip__` header, every local header at its
  planned offset (signature, flags, STORE, sizes or ZIP64 extra, name), contiguity, and a
  comment-free EOCD. A mismatch removes the file and returns `COZIP_ERR_INVALID_LFH`.
- The hash patch touches bytes 43-50 only and checks `fclose`; a failure removes the file.
- If removal itself fails the call returns `COZIP_ERR_IO` with `invalid output may remain`.
- Two calls must not write the same output path at the same time, and nothing is
  `fsync`ed; give each writer its own temporary path and rename it yourself.

## 8. Constants

| Macro | Value |
| --- | --- |
| `COZIP_FORMAT_VERSION` | 1 |
| `COZIP_INDEX_OFFSET` | 51 |
| `COZIP_HASH_WINDOW_SIZE` | 32768 |
| `COZIP_MIN_ARCHIVE_SIZE` | 32819 |
| `COZIP_INDEX_HEADER_SIZE`, `COZIP_INDEX_PER_ENTRY_OVERHEAD` | 11, 18 |
| `COZIP_INDEX_NAME`, `COZIP_PADDING_NAME`, `COZIP_FLAT_METADATA_NAME` | `__cozip__`, `__cozip_padding__`, `__metadata__` |
| `COZIP_TACO_COLLECTION_NAME`, `COZIP_TACO_METADATA_DIR`, `COZIP_TACO_PARQUET_SUFFIX` | `COLLECTION.json`, `METADATA/`, `.parquet` |
| `COZIP_MAGIC` | `"CZIP"` |
| `COZIP_EXTRA_HEADER_ID`, `COZIP_EXTRA_DATA_SIZE`, `COZIP_EXTRA_FIELD_SIZE` | `0xCA0C`, 8, 12 |
| `COZIP_FNV_OFFSET_BASIS`, `COZIP_FNV_PRIME` | `0xCBF29CE484222325`, `0x100000001B3` |
| `COZIP_ERROR_MESSAGE_SIZE` | 192 |
| `COZIP_TACO_PLAN_VERSION`, `COZIP_TACO_PLAN_INIT` | 1, the initializer for `cozip_taco_plan_t` |

## 9. Notes for FFI bindings

- On 64-bit targets `sizeof(cozip_entry_t)` is 72 (`source` at 24, `lfh_offset` at 48),
  `cozip_error_t` 196, `cozip_path_entry_t` 32 and `cozip_taco_plan_t` 32.
- Enums cross the ABI as `int`. Python mirrors the structs in the cffi `cdef` of
  `python/cozip/_core.py`, Julia in `julia/src/LibCozip.jl` with explicit padding fields,
  R through `r/src/cozip_glue.c`. No test compares them with `cozip.h`, so a struct change
  must update all three by hand.
- Read reserved names through the accessors (`cozip_index_name()` and friends) instead of
  copying the strings; R and Julia cache them at load time.
