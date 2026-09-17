---
name: cozip
description: >-
  Use cozip or work on its codebase: pack files and a metadata table into a
  Cloud-Optimized ZIP (byte-0 index plus Parquet manifest) from Python, R,
  Julia or C; read manifests with cozip.read, DuckDB read_flat or the
  JavaScript reader; open entries by byte range or GDAL /vsisubfile/; build
  GeoParquet manifests; validate archives against SPEC.md; drive the TACO
  profile plan/write ABI; or edit cozip's C core, bindings, tests and releases.
  Do not use for ordinary ZIP work or for TACO dataset semantics.
---

# cozip

A cozip is an ordinary ZIP whose first entry, `__cozip__`, stores a small index at byte 51
with the offset and size of a few priority files. In the Flat profile the only priority
file is `__metadata__`, a Parquet manifest with one row per file: `name`, `offset`, `size`
and any columns you add. A reader fetches the index and the manifest, filters rows, then
reads each file as one byte range. Entries are stored uncompressed, so
`[offset, offset + size)` is exactly the original file, and `unzip` still works.

This skill describes **cozip 2026.9.9** (SPEC.md 0.1.0, binary format version 1). Check
`cozip.__version__`, `packageVersion("cozip")` or `Cozip.LibCozip.cozip_version()`. If it
differs, trust the installed source and `CHANGELOG` over this file.

## Mental model

- One C library (`core/cozip.c`, with libzip and zlib vendored) writes every archive.
  Python, R and Julia hand it names, sizes and paths; it plans offsets, validates names,
  writes the entries, re-reads the headers it wrote and patches the integrity hash.
- Writing has two stages. `stage_metadata` returns each file's final offset before anything
  is written; you write the manifest Parquet with any tool and options; `stage_create`
  embeds that file verbatim. `write` runs both stages with default Parquet options.
- Reading does not use the C library. `read()` in Python, R and Julia runs the DuckDB
  `cozip` community extension (`read_flat`); JavaScript parses the index itself. Readers
  add `cozip:location`, a GDAL `/vsisubfile/` path, at read time. It is never stored.
- The profile is one byte of the index: 0 none, 1 Flat, 2 TACO. The readers in this
  repository accept only Flat. TACO archives are written by the TACO package through a
  private cozip API.

## Canonical workflow

```python
import pyarrow as pa
import cozip

table = pa.table({
    "name": ["tiles/a.tif", "tiles/b.tif"],   # names inside the archive: ASCII, relative, unique
    "path": ["/data/a.tif", "/data/b.tif"],   # local sources: non-empty regular files
    "split": ["train", "val"],                # any other column rides along
})
archive = cozip.write("dataset.zip", table)   # returns the absolute output path

manifest = cozip.read(archive)                # pandas: name, offset, size, split, cozip:location
row = manifest[manifest["split"] == "train"].iloc[0]
with open(archive, "rb") as f:
    f.seek(int(row["offset"]))
    data = f.read(int(row["size"]))           # the exact bytes of /data/a.tif
# rasterio.open(row["cozip:location"]) opens the same file through GDAL
```

Publish the `.zip` on any server or bucket that honours `Range` requests, then read the
same manifest with `cozip.read("https://...")`, DuckDB or the JavaScript reader.

## Choosing an API

| Task | Use |
| --- | --- |
| Files plus attribute columns | `cozip.write(out, table)`; R `create()`; Julia `Cozip.write` |
| Manifest written by your own tool or options (geopandas, zstd, row groups) | `stage_metadata`, write the Parquet yourself, `stage_create` |
| Manifest as a data frame | `read(path_or_url, columns=, location=)` in Python, R or Julia |
| SQL over one or many archives | DuckDB `read_flat()` |
| Browser or Node | `read(url)` from `@asterisk-labs/cozip` |
| One file inside an archive | `cozip:location` in GDAL, or bytes `offset` to `offset + size - 1` |
| C, or a language without a binding | `cozip_plan_flat` then `cozip_write_flat`, or `cozip_finalize` |
| TACO dataset container | the TACO package, which calls `cozip._taco` over `cozip_plan_taco` and `cozip_write_taco` |
| Is this file a conforming cozip? | `python scripts/check_cozip.py archive.zip` ([scripts/check_cozip.py](scripts/check_cozip.py)) |

## Invariants and pitfalls

- Archive names are ASCII, relative, `/`-separated and unique, with no `.` or `..`
  component, no trailing `/` and no `\`. `__cozip__` and `__cozip_padding__` are reserved,
  and the Flat writers also reserve `__metadata__`. Source and output paths may be
  Unicode; paths and URLs given to `read()` may not.
- Every entry needs at least one byte. Empty files fail with `zero-byte payload`; drop them
  or give them content before writing.
- The input table needs `name` and `path` and must not contain `offset`, `size`,
  `cozip:location` or `taco:location`. The manifest Parquet needs `name`, `offset` and
  `size` and must not contain `path` or either location column.
- Offsets depend on the order and names of every earlier entry. `stage_metadata` returns
  `paths` aligned with its rows: reorder or filter both together, or sort the input table
  before staging. Keep `validate=True` in `stage_create` unless the Parquet is untouched.
- A finished archive is immutable. Re-zipping, `zip -u`, appending, recompressing or adding
  a ZIP comment breaks the integrity hash (index plus last 32 KiB) or the stored offsets.
  Rebuild with cozip instead.
- Serve the bytes unchanged: `Range` support, no transparent `Content-Encoding`, and for
  browsers CORS that allows `Range` and exposes `Content-Range`.
- Archives smaller than 32,819 bytes get a `__cozip_padding__` entry. ZIP tools list it;
  the manifest does not.
- `read()` accepts only Flat archives and installs the DuckDB extension from the community
  repository on first use. As of 2026-09-17 the build served for DuckDB 1.5.5 is extension
  2.0.1, whose SQL spells the option `gdal_vsi :=` and the column `cozip:gdal_vsi`; the
  Python, R and Julia wrappers normalize both to `cozip:location`, except that with this
  extension passing `columns` loses the location column. Read all columns, then subset.
- `cozip:location` repeats the path you read from, so a relative path gives a relative
  location. JavaScript returns `offset` and `size` as `BigInt`. Julia exports only
  `create`, `stage_metadata` and `stage_create`: call `Cozip.write` and `Cozip.read`.
- In C, `cozip_finalize`, `cozip_plan_flat` and `cozip_write_flat` use the caller's entry
  array as scratch: they fill the spare slots (`__metadata__`, padding), and
  `cozip_finalize` with profile 2 shifts the priority entries by one.

## Reference map

Read only the reference relevant to the current task. Each one names its sources in the
repository and is scoped to cozip 2026.9.9.

| Task | Read |
| --- | --- |
| Byte layout, index, integrity hash, names, padding, ZIP64, profiles, error codes, validation | [references/format.md](references/format.md) |
| Python: `write`, `stage_metadata`, `stage_create`, `read`, errors, GeoParquet manifests | [references/python-api.md](references/python-api.md) |
| R: `create`, staging functions, `read`, locale rules | [references/r-api.md](references/r-api.md) |
| Julia: `Cozip.write`, staging functions, `Cozip.read`, artifacts | [references/julia-api.md](references/julia-api.md) |
| JavaScript reader for browsers and Node | [references/javascript-api.md](references/javascript-api.md) |
| DuckDB extension: `read_flat`, versions, SQL recipes, private buckets | [references/duckdb-reader.md](references/duckdb-reader.md) |
| Opening entries with GDAL or byte ranges, publishing, HTTP requirements, dataloaders | [references/access-and-publishing.md](references/access-and-publishing.md) |
| C ABI: pipeline levels, capacity rules, compiled examples, linking | [references/c-api.md](references/c-api.md) |
| TACO profile: the plan/write contract and `cozip._taco` | [references/taco-profile.md](references/taco-profile.md) |
| Repository layout, build, tests, sync rules, spec changes, releases | [references/contributing.md](references/contributing.md) |
| Error message lookup, library loading, extension drift, hash mismatches | [references/debugging.md](references/debugging.md) |
