# Python API

Everything here is `cozip` 2026.9.17 as implemented in `python/cozip/__init__.py`,
`_core.py`, `_writer.py` and `_reader.py`. Examples and messages were captured from that
build with pyarrow 24, pandas 3 and DuckDB 1.5.5.

## Contents

1. Install and runtime
2. `write` and `create`
3. `stage_metadata`
4. `stage_create`
5. `read`
6. Errors
7. `lib`, `ffi` and constants
8. Patterns: GeoParquet manifests, pandas input, row order, payload access, private buckets

## 1. Install and runtime

```bash
pip install cozip
```

- Python 3.10 or newer. Dependencies: `cffi>=1.16`, `pyarrow>=14`, `duckdb>=1.5.2`,
  `pandas>=2.0`.
- Wheels are tagged `py3-none` (cffi loads the library with `dlopen`, no CPython ABI) for
  manylinux 2.28 x86-64 and aarch64, macOS 11 universal2 and Windows x86-64. The sdist
  carries `core/` and compiles the library with CMake and Ninja.
- `import cozip` loads `cozip/_lib/cozip.{so,dylib,dll}` immediately (no `lib` prefix).
  `COZIP_LIB_PATH=/abs/path/cozip.dylib` overrides the lookup. The writer imports pyarrow
  on first use of a writer name; the reader imports pandas on `cozip.read` and DuckDB only
  when called. `cozip._taco` imports none of them.
- `COZIP_EXTENSION=/abs/path/cozip.duckdb_extension` makes `read()` load a locally built
  DuckDB extension (unsigned extensions allowed) instead of the community one.

Public names: `write`, `create`, `stage_metadata`, `stage_create`, `read`, `CozipError`,
`lib`, `ffi`, `__version__`. `cozip.write is cozip.create`.

## 2. `write` and `create`

```text
cozip.write(out_path, table, temp_dir=None) -> str
```

Runs `stage_metadata(table)`, writes the manifest with `pyarrow.parquet.write_table`
defaults into a temporary file under `temp_dir` (created if missing; system temp dir by
default), calls `stage_create(..., validate=False)` and deletes the temporary file.

| Argument | Meaning |
| --- | --- |
| `out_path` | Destination, `str` or path-like, Unicode allowed. Existing files are replaced. Must not be one of the sources. |
| `table` | `pyarrow.Table` with `name` and `path` columns; every other column goes into the manifest. |
| `temp_dir` | Where the temporary manifest Parquet is written. |

Returns the resolved absolute output path. Use the staging functions when the manifest
needs its own writer or options (geopandas, zstd, row group size).

libzip writes to a temporary file and renames it over `out_path` only after success, so a
failure before that point leaves an existing file untouched. If the post-write check or the
hash patch fails after the rename, the new file is removed.

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

Sizes are read with `stat()` at this point, and `stage_create` stats the files again. With
`validate=True` a file that changed in between surfaces as an offset or size mismatch.
With `validate=False`, which `write` uses internally, the archive is written with the new
sizes while the manifest keeps the old offsets (verified), so leave sources untouched until
the call returns.

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

- Always checked, whatever `validate` says: the Parquet has `name`, `offset` and `size`,
  and has neither `path` nor a location column.
- With `validate=True` a NULL, a row count difference or any name, offset or size mismatch
  raises `ValueError` naming the first bad row. The comparison casts `offset` and `size`
  to uint64, so int64 columns pass; spec 13.3 asks for uint64, which `stage_metadata`
  produces, so keep that type when you rebuild the table.
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
| `source` | Local path or URL that DuckDB can open (`https://`, `s3://`, `gs://`, `hf://`, `azure://`). Non-empty, ASCII, no NUL. |
| `columns` | `None` returns every column. A list returns `name`, `offset`, `size`, the location column when enabled, then the listed extras without duplicates; `cozip:location` and `taco:location` in the list are ignored. |
| `location` | `bool`. `False` omits `cozip:location`. |

- Each call opens a new in-memory DuckDB connection, runs
  `INSTALL cozip FROM community; LOAD cozip;` (network on first use), queries
  `read_flat(?, location := ...)` and closes the connection.
- Older extensions are handled: when `read_flat` rejects `location :=` it retries with
  `gdal_vsi :=`, and when `read_flat` does not exist it uses `read_cozip`. A legacy
  `cozip:gdal_vsi` column is renamed to `cozip:location`; a `taco:location` column is
  always dropped. The community build for DuckDB 1.5.5 needs the first retry.
- Projected reads keep the normalized `cozip:location` column on both the current and
  legacy extension signatures.
- Only Flat archives are read. Profile 0 and TACO archives raise DuckDB's
  `InvalidInputException` (`read_flat needs a Flat-profile archive (profile=1). Got
  profile=taco ...`).
- Types: `offset` and `size` are `uint64`; strings; binary columns and GeoParquet
  geometry arrive as `bytearray` WKB without CRS.
- The location repeats `source` as given: `cozip.read("w1/out.zip")` yields
  `/vsisubfile/127_96,w1/out.zip`. HTTP(S) sources get `/vsicurl/`; see `duckdb-reader.md`
  for other schemes.

Argument errors are raised before DuckDB is imported.

## 6. Errors

`CozipError` (a `RuntimeError`) comes from libcozip: `str(e)` is `[NAME] message`, with
`e.code` (int), `e.name` (`INVALID_ARGUMENT`, `IO`, `INVALID_LFH`, ...) and `e.message`.
Everything the binding checks itself raises `TypeError`, `ValueError` or
`FileNotFoundError` with a `cozip: ` prefix. `read()` lets DuckDB exceptions through. The
full message table is in `debugging.md`; the common ones:

| Message (excerpt) | Fix |
| --- | --- |
| `AttributeError: 'DataFrame' object has no attribute 'column_names'` | pass a `pyarrow.Table`, not pandas |
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
  `cozip.ffi.string(cozip.lib.cozip_version_string()).decode()` gives `'2026.9.17'`.
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

### Private buckets

`read()` opens a fresh DuckDB connection per call, so a secret created on your own
connection is not visible to it. A persistent secret is: new connections load it from
`~/.duckdb/stored_secrets`.

```python
import duckdb
duckdb.connect().execute(
    "CREATE PERSISTENT SECRET my_bucket (TYPE s3, KEY_ID '...', SECRET '...', "
    "REGION 'us-west-2', SCOPE 's3://my-bucket')")
cozip.read("s3://my-bucket/dataset.zip")
```

Or query `read_flat` on your own connection (`duckdb-reader.md`).

### Version checks

```python
cozip.__version__                                         # package metadata, '2026.9.17'
cozip.ffi.string(cozip.lib.cozip_version_string()).decode()  # loaded native library
```
