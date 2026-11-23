# Python API

Everything here is `cozip` 2026.11.23 as implemented in `python/cozip/__init__.py`,
`_core.py`, `_writer.py` and `_reader.py`, with the native reader in `core/cozip_read.c`.
Examples and messages were captured from that build with pyarrow 24, pandas 3 and DuckDB
1.5.5.

## Contents

1. Install and runtime
2. `write` and `create`
3. `stage_metadata`
4. `stage_create`
5. `read`
6. Errors
7. `lib`, `ffi` and constants
8. Patterns: GeoParquet manifests, pandas input, row order, payload access, private data

## 1. Install and runtime

```bash
pip install cozip
```

- Python 3.10 or newer. Dependencies: `cffi>=1.16`, `pyarrow>=14`, `duckdb>=1.5.2`,
  `pandas>=2.0`.
- Wheels are tagged `py3-none` (cffi loads the library with `dlopen`, no CPython ABI) for
  manylinux 2.28 x86-64 and aarch64, macOS 11 arm64 and x86-64 (one wheel each) and
  Windows x86-64. Their library links libcurl and OpenSSL statically. The sdist carries
  `core/` with karu and compiles the library with CMake and Ninja, which also needs a C++23
  compiler, libcurl and OpenSSL.
- `import cozip` loads `cozip/_lib/cozip.{so,dylib,dll}` immediately (no `lib` prefix).
  `COZIP_LIB_PATH=/abs/path/cozip.dylib` overrides the lookup. The writer imports pyarrow
  on first use of a writer name; the reader imports pandas on `cozip.read` and DuckDB only
  when called. `cozip._taco` imports none of them.
- DuckDB is only the Parquet decoder for `read()`. No DuckDB extension is installed or
  loaded, and `COZIP_EXTENSION` is ignored.

Public names: `write`, `create`, `stage_metadata`, `stage_create`, `read`, `CozipError`,
`lib`, `ffi`, `__version__`. `cozip.write is cozip.create`.

## 2. `write` and `create`

```text
cozip.write(out_path, table, temp_dir=None) -> str
```

Plans once, writes a temporary manifest with `pyarrow.parquet.write_table` (zstd, and per column the
smallest of dictionary, plain, byte stream split or delta on the first 10,000 rows),
then packs the archive with those offsets. The C writer checks source sizes again and
raises `CozipError` if one changed. The temporary manifest is deleted on return.

| Argument | Meaning |
| --- | --- |
| `out_path` | Destination, `str` or path-like, Unicode allowed. Existing files are replaced. Must not be one of the sources. |
| `table` | `pyarrow.Table` with `name` and `path` columns; every other column goes into the manifest. |
| `temp_dir` | Where the temporary manifest Parquet is written. |

Returns the resolved absolute output path. Use the staging functions when the manifest
needs its own writer or options (geopandas, zstd, row group size).

The archive is verified and hashed under a temporary name. Only then does
`cozip_finalize` rename it over `out_path`; a failed write leaves an existing output
untouched.

## 3. `stage_metadata`

```text
cozip.stage_metadata(table) -> tuple[pyarrow.Table, list[tuple[str, str]]]
```

Plans the layout without writing anything and returns:

- the manifest table: `name`, `offset` (uint64), `size` (uint64), then the extra columns
  in their original order. `path` is dropped. Schema metadata on `table` is kept, so a
  `geo` key survives.
- `paths`: `(name, path)` pairs in the same row order, exactly as given, ready for
  `stage_create`.

Checks, in order: `name` and `path` present; none of `offset`, `size`, `cozip:location`,
`taco:location` present; at least one row; every name a `str` without NUL, not
`__cozip__`, `__metadata__` or `__cozip_padding__`, not repeated; every path exists and is
a regular file. The C planner (`cozip_plan_flat`) then enforces name syntax and ASCII and
rejects zero-byte files.

Sizes are read with `stat()` at this point. `stage_create` stats the files again; with
`validate=True` a file that changed in between surfaces as an offset or size mismatch.

Column names are compared without case, as DuckDB, which decodes manifests, compares
them: two columns that differ only in case (`label` and `Label`) are refused, and so is
any case of `offset`, `size`, `cozip:location` or `taco:location`. Columns pandas adds for
an unnamed index (`__index_level_0__`) and the `pandas` schema key are dropped from the
manifest; a pandas `DataFrame` passed directly raises `TypeError` with the conversion.

Offsets for a given table (verified):

```python
meta, paths = cozip.stage_metadata(pa.table({
    "name": ["a.txt", "data/b.bin"], "path": [a, b], "split": ["train", "val"]}))
meta.to_pylist()
# [{'name': 'a.txt', 'offset': 127, 'size': 96, 'split': 'train'},
#  {'name': 'data/b.bin', 'offset': 263, 'size': 40960, 'split': 'val'}]
```

## 4. `stage_create`

```text
cozip.stage_create(out_path, paths, metadata_parquet, validate=True) -> str
```

| Argument | Meaning |
| --- | --- |
| `out_path` | Destination archive; resolved to an absolute path, which is returned. |
| `paths` | Sequence of `(name, source_path)` pairs in manifest row order. |
| `metadata_parquet` | Existing, non-empty Parquet file embedded byte for byte as `__metadata__`. |
| `validate` | `bool`. `True` reads `name`, `offset`, `size` from the Parquet and compares them with a fresh plan. |

- Always checked, whatever `validate` says: the Parquet has `name` (string), `offset`
  and `size` (uint64, spec 13.3), no two columns that differ only in case, and neither
  `path` nor a location column in any case. An int64 `offset` is refused; keep the uint64
  columns `stage_metadata` returns when you rebuild the table.
- With `validate=True` a NULL, a row count difference or any name, offset or size mismatch
  raises `ValueError` naming the first bad row.
- `validate=False` skips only the value comparison. A wrong manifest then produces a valid
  ZIP whose rows point at the wrong bytes.
- The Parquet is never parsed beyond these columns or rewritten: compression, row groups,
  key-value metadata and extra columns are preserved.

## 5. `read`

```text
cozip.read(source, columns=None, location=True) -> pandas.DataFrame
```

| Argument | Meaning |
| --- | --- |
| `source` | Local path, `file://`, `http(s)://`, `s3://` (also `s3a://`, `s3n://`), `gs://`, `gcs://`, `az://`, `azure://`, `abfs://`, `abfss://`, `hf://` or `source://` URL, or a GDAL `/vsi` path. `r2://` is not readable. Non-empty, ASCII, no NUL. A leading `~/` is expanded, for the bytes and in the location. |
| `columns` | `None` returns every column. A list returns `name`, `offset`, `size`, the location column when enabled, then the listed extras without duplicates. Names match exactly; an unknown name raises `ValueError`; `cozip:location`, `taco:location` and `cozip:gdal_vsi` in the list are ignored in any case. |
| `location` | `bool`. `False` omits `cozip:location`. |

- libcozip reads the byte-0 index through karu, verifies it (LFH, magic and version,
  the Flat shape, the integrity hash, the end record, the manifest range) and fetches only
  `__metadata__`. The first 64 KiB and the last 32 KiB are requested at once; a manifest in
  either, or in both when they meet, is not fetched again. Otherwise one more range reads
  the manifest with the local header before it, which must match the index
  (`MISSING_ENTRY`). Later reads use `If-Match` when the first response has a strong ETag.
- The manifest goes to a temporary file that a per-process in-memory DuckDB database
  decodes through a cursor of its own, with `autoinstall_known_extensions` and
  `autoload_known_extensions` off and Hive partitioning off, so a `key=value` directory in
  the path adds no column. A forked child opens its own database.
- Stored `cozip:location`, `cozip:gdal_vsi` and `taco:location` columns are dropped in any
  case and the location is computed again. A manifest whose remaining columns differ only
  in case raises `ValueError`, since DuckDB cannot tell them apart.
- Only Flat archives are read. Profile 0 and TACO archives raise `CozipError`
  (`[PROFILE] read_flat needs a Flat-profile archive (profile=1). Got profile=2 in: ...`).
- Types follow DuckDB. `offset` and `size` are `uint64`, strings use the pandas string
  dtype, integer and boolean columns with nulls use pandas nullable dtypes such as `Int32`
  and `boolean`, and binary columns and GeoParquet geometry arrive as `bytearray` WKB
  without CRS.
- The location repeats a relative `source` as given: `cozip.read("w1/out.zip")` yields
  `/vsisubfile/127_96,w1/out.zip`. Remote sources map to `/vsicurl/`, `/vsis3/`, `/vsigs/`,
  `/vsiaz/` and `/vsiadls/` (table in `duckdb-reader.md` section 6); `hf://` becomes a
  `/vsicurl/` `resolve` URL that keeps an explicit `@revision` and percent-encodes the
  file path; `source://` becomes `/vsicurl/https://data.source.coop/...`; `file://` becomes
  the local path and `~/` the expanded one. Unlike the DuckDB extension, `read()` also maps
  `az://` to `/vsiaz/`, `abfs://` to `/vsiadls/` and `s3a://` and `s3n://` to `/vsis3/`.
  `r2://` and `/vsi` paths stay unchanged.

Argument errors are raised before anything is read.

`cozip.verify(source) -> None` checks the access path of any profile without decoding its
manifest: header, archive size, index sections, names, ranges, integrity hash and end
record (`c-api.md` section 3). It takes the sources and argument checks of `read`
(`cozip.verify: ...` messages), lives in `cozip/_verify.py` with no pandas or DuckDB
import, returns `None` and raises `CozipError` named after the broken rule. Over HTTP
it makes two range requests, the head and the tail, plus one for an index past 64 KiB.

## 6. Errors

`CozipError` (a `RuntimeError`) comes from libcozip: `str(e)` is `[NAME] message`, with
`e.code` (int), `e.name` (`INVALID_ARGUMENT`, `IO`, `INVALID_LFH`, ...) and `e.message`.
Everything the binding checks itself raises `TypeError`, `ValueError` or
`FileNotFoundError` with a `cozip: ` prefix. `read()` raises `CozipError` for index and
transport problems, named as in spec section 9 where the spec names them (`INVALID_LFH`,
`ARCHIVE_TOO_SMALL`, `INVALID_MAGIC`, `UNSUPPORTED_VERSION`, `TRUNCATED_INDEX`,
`HASH_MISMATCH`, `INVALID_OFFSET`, `MISSING_ENTRY`, `INVALID_ZIP_STRUCTURE`), plus
`PROFILE` for a non-Flat archive, `INVALID_INDEX` for an index that is not the 41-byte
Flat index, `NOT_FOUND` and `IO`. An unknown `columns` entry raises `ValueError`. The full message table is in `debugging.md`; the common ones:

| Message (excerpt) | Fix |
| --- | --- |
| `TypeError: cozip: table must be a pyarrow.Table, got DataFrame; convert it with pyarrow.Table.from_pandas(df, preserve_index=False)` | pass a `pyarrow.Table` |
| `ValueError: cozip: input table is missing required column(s): ['path']` | add `name` and `path` |
| `ValueError: cozip: input table must not contain reserved column(s) ['offset']` | drop `offset`, `size` and location columns from the input |
| `CozipError: [INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must be non-empty` | drop empty files |
| `CozipError: [INVALID_ARGUMENT] entry 0 archive name contains non-ASCII bytes; ...` | transliterate or percent-encode names |
| `CozipError: [INVALID_ARGUMENT] output path is also the source path for entry 0 ('a')` | write somewhere else |
| `ValueError: cozip: name mismatch at row 0: parquet='a', paths='b'` | reorder `paths` like the manifest |
| `ValueError: cozip: metadata parquet must not contain a 'path' column. ...` | drop `path` before writing the Parquet |

## 7. `lib`, `ffi` and constants

- `cozip.lib` exposes every C function through a hand-written cffi `cdef` in `_core.py`
  (ABI mode, nothing checks it against `cozip.h`). Strings come back as `char *` cdata:
  `cozip.ffi.string(cozip.lib.cozip_version_string()).decode()` gives `'2026.11.23'`.
- Status, profile and source constants are plain integers in `cozip._core`
  (`COZIP_OK`, `COZIP_ERR_INVALID_ARGUMENT` = 100, `COZIP_ERR_IO` = 102,
  `COZIP_PROFILE_FLAT` = 1, `COZIP_SOURCE_PATH` = 1, ...), not attributes of `lib`.
- `cozip._taco` is the private TACO adapter (`taco-profile.md`); applications should not
  import it.

## 8. Patterns

### GeoParquet manifest with geopandas

This is how the public fixtures are generated (`cozip-api-fixtures/generate/generate.py`):

```python
import geopandas as gpd
import pyarrow as pa
import cozip

metadata, paths = cozip.stage_metadata(pa.table({"name": names, "path": files, "label": labels}))
gdf = gpd.GeoDataFrame(metadata.to_pandas(), geometry=geometries, crs="EPSG:4326")
gdf.to_parquet("metadata.parquet", index=False, compression="zstd", schema_version="1.1.0")
cozip.stage_create("dataset.zip", paths, "metadata.parquet")
```

`to_pandas()` keeps `offset` and `size` as uint64 and geopandas writes them back unchanged.
Readers that understand GeoParquet (DuckDB, geopandas, the JavaScript reader, the
playground map) see the geometry; others see an ordinary Flat manifest.

### GeoParquet manifest with pyarrow only

```python
import json
import shapely

geo = {"version": "1.1.0", "primary_column": "geometry",
       "columns": {"geometry": {"encoding": "WKB", "geometry_types": ["Polygon"]}}}
table = pa.table({
    "name": names,
    "path": files,
    "geometry": [shapely.to_wkb(g) for g in geometries],
}).replace_schema_metadata({"geo": json.dumps(geo)})
cozip.write("dataset.zip", table)
```

Omitting `crs` means OGC:CRS84 (longitude, latitude); `"crs": None` means unknown. Give a
PROJJSON object for anything else.

To get a GeoDataFrame back from `cozip.read` (the CRS is not carried):

```python
df = cozip.read("dataset.zip")
gdf = gpd.GeoDataFrame(df, geometry=gpd.GeoSeries.from_wkb(df["geometry"].map(bytes)), crs="OGC:CRS84")
```

### Input from pandas

```python
table = pa.Table.from_pandas(df, preserve_index=False)
```

Without `preserve_index=False`, a named or non-range index becomes a manifest column
(`__index_level_0__` or the index name).

### Row order

Entries are written in table order, so sort the input before staging to keep related
files adjacent (by split, tile id or time) and to make contiguous range reads possible:

```python
table = table.sort_by([("split", "ascending"), ("name", "ascending")])
metadata, paths = cozip.stage_metadata(table)
```

If you sort or filter `metadata` afterwards, apply the same permutation to `paths` or call
`stage_metadata` again; `stage_create(validate=True)` rejects a mismatch.

### Reading payloads

```python
manifest = cozip.read("https://example.org/dataset.zip", columns=["split"])
for row in manifest[manifest["split"] == "train"].itertuples():
    start, end = int(row.offset), int(row.offset) + int(row.size) - 1   # HTTP Range is inclusive
```

Local files use `seek`/`read`; GDAL uses the `cozip:location` column; object stores use
range requests. Worked examples with `requests`, `obstore` and rasterio are in
`access-and-publishing.md`.

### Private data

`read()` takes credentials from the environment and the standard credential files, as
karu discovers them (`core/karu/CONFIGURATION.md`). DuckDB secrets are not used.

| Store | Settings |
| --- | --- |
| Hugging Face | `HF_TOKEN`, else the token file from `huggingface-cli login` (`$HF_TOKEN_PATH`, `$HF_HOME/token`, `~/.cache/huggingface/token`) |
| S3 | `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`, `AWS_PROFILE` with `~/.aws`, web identity, container or instance metadata credentials |
| GCS | `GOOGLE_APPLICATION_CREDENTIALS`, gcloud's ADC file, or metadata credentials |
| Azure | `AZURE_STORAGE_CONNECTION_STRING`, or `AZURE_STORAGE_ACCOUNT` with `AZURE_STORAGE_ACCESS_KEY`, `AZURE_STORAGE_SAS_TOKEN` or a client secret |

```python
import os
os.environ["HF_TOKEN"] = "hf_..."              # or log in once with huggingface-cli
cozip.read("hf://datasets/org/private-repo@v1/data/dataset.zip")
```

GDAL needs its own credentials to open the resulting locations.

### Version checks

```python
cozip.__version__                                         # package metadata, '2026.11.23'
cozip.ffi.string(cozip.lib.cozip_version_string()).decode()  # loaded native library
```
