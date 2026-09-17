# TACO profile writer contract

Sources: `SPEC.md` section 14, `docs/taco-writer-internal.md`, `core/cozip.c` section 11,
`python/cozip/_taco.py`, `core/tests/test_taco.c`, `python/tests/test_taco_internal.py`,
and the consumer in the TACO package (`taco/writer/archive.py`, PyPI `taco-eo`). Messages
and offsets below were captured from cozip 2026.9.17.

## Contents

1. Who owns what
2. Profile rules
3. The C ABI
4. The Python adapter `cozip._taco`
5. Example
6. Failures
7. Publication and concurrency
8. Changing the contract
9. Reading TACO archives

## 1. Who owns what

| TACO package | libcozip |
| --- | --- |
| validates the dataset contract and assigns archive names | validates physical names (ASCII, syntax, reserved, unique) |
| creates `COLLECTION.json` and `METADATA/*.parquet`, writing the planned offsets into them (`internal:offset`, `internal:size`) | stats sources, plans offsets, writes STORE entries |
| stages files, chooses the temporary output, publishes with overwrite policy, partitions datasets | places the final priority block, adds padding before it, verifies local headers, patches the hash |
| JSON and Parquet schemas and contents | nothing: payloads are opaque bytes; libcozip links no Arrow, Parquet or JSON library |

Keep TACO semantics out of cozip. Only rules from spec 14.2 and 14.3 that need no payload
inspection belong in `core/cozip.c`.

## 2. Profile rules

- Profile byte 2. Priority files (the index) must include `COLLECTION.json` and every
  Parquet file under `METADATA/` (spec 14.2). Checks are by name only: `METADATA/.parquet`
  counts, `METADATA/notes.txt` and `DATA/0/table.parquet` may stay non-priority.
- The priority files form one contiguous final block right before the Central Directory;
  their relative order is free, and nothing follows them (14.3).
- `__cozip_padding__`, when needed, goes immediately before that block, so padding never
  moves `DATA/` payloads.
- A reader may fetch every priority file with one range, from the smallest priority offset
  to the largest offset plus size.
- Stored metadata must not contain `cozip:location` or `taco:location`; readers emit
  `taco:location` (14.5).
- The extension stays `.zip` (14.6). Do not introduce a TACO-specific extension; the profile
  byte is the signal.

## 3. The C ABI

```c
cozip_taco_plan_t plan = COZIP_TACO_PLAN_INIT;   /* struct_size and abi_version set */

cozip_status_t cozip_plan_taco(cozip_path_entry_t *files, size_t n_files,
                               const cozip_path_entry_t *priorities, size_t n_priorities,
                               cozip_taco_plan_t *out_plan, cozip_error_t *err);

cozip_status_t cozip_write_taco(const char *out_path,
                                const cozip_path_entry_t *files, size_t n_files,
                                const cozip_path_entry_t *priorities, size_t n_priorities,
                                const cozip_taco_plan_t *plan, cozip_error_t *err);
```

`cozip_path_entry_t` is `{arc_name, source_path, payload_offset, payload_size}`.

- Plan: `files` are the materialized non-priority entries in archive order; `priorities`
  are ordered names whose sources may be `NULL` because those files do not exist yet.
  Priority names fix the index size, and priority sizes cannot move earlier entries, so
  the call fills `payload_offset` and `payload_size` for every file and a
  `cozip_taco_plan_t`.
- Write: the same files and the same ordered priority names, now with existing, non-empty
  sources. libcozip re-stats every source, recomputes the layout, compares it with the
  plan and with the offsets echoed back in `files`, rejects an output that aliases any
  source, and runs `cozip_finalize` with profile 2. The caller passes no spare capacity.
- `cozip_taco_plan_t` is `{struct_size, abi_version, n_files, n_priorities, layout_hash}`.
  Both calls refuse a struct whose `struct_size` or `abi_version` (1) differs.
- `layout_hash` is FNV-1a 64 over a domain string, the counts, and each entry's role, name
  and (for files) size. It is a stale-plan check, not a content hash: a same-size edit of a
  source passes, and a source can still change after the final stat. Stage immutable
  sources when that matters.

## 4. The Python adapter `cozip._taco`

Private to the TACO package; applications use TACO. Importing it loads libcozip but not
pyarrow, pandas or DuckDB.

```text
cozip._taco.API_VERSION == 1
plan(files, priority_names) -> Plan
write(output, layout, priority_files) -> str
```

- `files`: ordered `(archive_name, source_path)` pairs; paths are resolved to absolute
  `Path`s. `priority_names`: ordered names.
- `Plan` is a frozen dataclass with `files` (tuple of `PlannedFile(name, source, offset,
  size)`), `priority_names`, a private copy of the native struct, and
  `offsets -> {name: (offset, size)}`. It is an in-process value, not a file format.
- `write` checks in Python that the names and order of `priority_files` equal
  `layout.priority_names`, rebuilds the native struct from the plan and calls
  `cozip_write_taco`. It returns the resolved output path.

## 5. Example

```python
from pathlib import Path
from cozip._taco import plan, write

Path("image.tif").write_bytes(b"I" * 1000)
Path("mask.tif").write_bytes(b"M" * 500)
layout = plan(
    [("DATA/0/image.tif", "image.tif"), ("DATA/0/mask.tif", "mask.tif")],
    ["COLLECTION.json", "METADATA/collection.parquet"],
)
layout.offsets   # {'DATA/0/image.tif': (186, 1000), 'DATA/0/mask.tif': (1231, 500)}

# TACO now writes COLLECTION.json and the Parquet using those offsets.
write("dataset.tmp", layout, [
    ("COLLECTION.json", "COLLECTION.json"),
    ("METADATA/collection.parquet", "collection.parquet"),
])
```

Resulting entry order: `__cozip__` (89-byte index), `DATA/0/image.tif`, `DATA/0/mask.tif`,
`__cozip_padding__`, `COLLECTION.json`, `METADATA/collection.parquet`; 32,819 bytes;
profile byte 2. Planning with no data files (`plan([], ["COLLECTION.json"])`) is allowed.

## 6. Failures

| Message (excerpt) | Cause |
| --- | --- |
| `[INVALID_ARGUMENT] TACO profile requires 'COLLECTION.json' among the priority files (cozip spec 14.2)` | missing collection name |
| `[INVALID_ARGUMENT] file 0 ('METADATA/x.parquet') is a METADATA Parquet and must be a priority file (cozip spec 14.2)` | metadata Parquet passed as a data file |
| `[INVALID_ARGUMENT] TACO requires at least one priority entry` | empty `priority_names` |
| `[INVALID_ARGUMENT] duplicate archive name 'same'` | a name in both lists, or twice |
| `[INVALID_ARGUMENT] TACO file 0 ('data') has a zero-byte source; cozip entries must be non-empty` | empty data file |
| `[INVALID_ARGUMENT] '<path>' is not a regular file` | directory or special file |
| `[IO] cannot stat '<path>': No such file or directory` | missing source |
| `ValueError: cozip._taco: priority_files names/order differ from plan; planned=[...], actual=[...]` | wrong names or order at write time |
| `[INVALID_ARGUMENT] TACO names/order or file sizes differ from plan` | a data file changed size, or a tampered plan |
| `[INVALID_ARGUMENT] TACO file 0 layout differs from plan` | offsets in `Plan.files` were edited |
| `[INVALID_ARGUMENT] output path is also source path for TACO entry 0` | writing over an input |
| `[INVALID_ARGUMENT] unsupported TACO plan struct size or ABI version` | C caller skipped `COZIP_TACO_PLAN_INIT` |

## 7. Publication and concurrency

- Give libcozip a unique temporary path in the destination directory and rename it into
  place after `write` returns; the TACO writer uses `mkstemp`, fixes permissions to the
  umask, then publishes with a no-replace rename unless `overwrite=True`.
- libcozip reopens the output to verify and patch it, so two calls must never target the
  same path at once.
- This is atomic publication, not durability: nothing is `fsync`ed.

## 8. Changing the contract

- Ground every rule in a spec section and cite it in the error message, as the existing
  `(cozip spec 14.2)` messages do. Flag spec ambiguities instead of settling them in code.
- Enforce physical rules in `core/cozip.c`, keep payloads opaque, and run `make sync` so
  `r/src/cozip.c` follows.
- Keep `docs/taco-writer-internal.md`, `core/tests/test_taco.c` and
  `python/tests/test_taco_internal.py` in step with the code.
- A change to `cozip_taco_plan_t` needs `COZIP_TACO_PLAN_VERSION` and `API_VERSION` bumped
  together, plus the cffi `cdef` in `python/cozip/_core.py`. The TACO package pins
  `cozip>=2026.9.4`; coordinate releases.
- Validate with `ctest`, `pytest python/tests`, a sanitizer build, and a real archive
  above 4 GiB when layout code changes (`contributing.md` section 3).

## 9. Reading TACO archives

`cozip.read`, R `read`, Julia `Cozip.read` and the JavaScript reader reject profile 2.
Read TACO datasets with the TACO package. The DuckDB extension's `read_taco` exists in
2.0.x but is removed on cozip_reader `main` (3.0.0), so do not build on it.
