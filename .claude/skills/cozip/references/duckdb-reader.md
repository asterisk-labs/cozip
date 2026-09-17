# DuckDB extension

The `cozip` DuckDB community extension lives in its own repository,
[asterisk-labs/cozip_reader](https://github.com/asterisk-labs/cozip_reader), and ships on
its own version line. This repository pins it as the `duckdb/` submodule, currently at
commit `97452ef` (the 1.2.0 era, behind the 2.0.1 build served for DuckDB 1.5.5), so read
the extension's own repository rather than the submodule. The Python, R and Julia `read()`
functions are wrappers around `read_flat`. Queries below ran on DuckDB 1.5.5 with the
community build `7af22df`, which is extension 2.0.1.

## Contents

1. Install, update and check the version
2. Versions and their SQL surface
3. `read_flat`
4. Recipes
5. Location values
6. Remote and private sources
7. Errors

## 1. Install, update and check the version

```sql
INSTALL cozip FROM community;
LOAD cozip;

SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'cozip';  -- '7af22df'
UPDATE EXTENSIONS (cozip);           -- or FORCE INSTALL cozip FROM community;
```

Community builds report a commit hash as their version. Linux, macOS and Windows are
supported; WebAssembly is excluded, so browsers use the JavaScript reader. The extension
reads only; archives are written with Python, R, Julia or C.

## 2. Versions and their SQL surface

| Extension | Flat reader | Location column | Other functions |
| --- | --- | --- | --- |
| 1.x | `read_cozip(path, gdal_vsi := true)` | `cozip:gdal_vsi` | 1.3.0 adds `cozip_profile(path)` returning `'none'`, `'flat'`, `'taco'` or `'unknown:N'` |
| 2.0.0, 2.0.1 | `read_flat(path, gdal_vsi := true)`; `read_cozip` kept | `cozip:gdal_vsi` | `read_taco` and `taco_*` helpers |
| 2.0.2 | `read_flat(path, location := true)`, `gdal_vsi :=` still accepted | `cozip:location` | `read_taco` emits `taco:location` |
| 3.0.0 (unreleased, cozip_reader `main`) | as 2.0.2, `read_cozip` kept | `cozip:location` | Flat only: `read_taco` and `taco_*` removed; `cozip_profile` returns the raw `UTINYINT` byte; `hf://` revisions preserved |

Check `DESCRIBE SELECT * FROM read_flat('x.zip')` before hard-coding a location column
name in SQL. Do not build on `read_taco`: TACO reading belongs to the TACO package and
leaves this extension in 3.0.0.

## 3. `read_flat`

```sql
SELECT * FROM read_flat('dataset.zip');
-- name VARCHAR, offset UBIGINT, size UBIGINT, <user columns>, cozip:gdal_vsi VARCHAR (2.0.1)
```

- A table macro: a scalar reads and verifies the byte-0 index (local header, integrity
  hash, sections, names, ranges, profile 1) and returns the manifest's offset and size;
  `read_parquet` then scans `__metadata__` through the extension's `cozip-subfile://`
  filesystem, so DuckDB's Parquet projection and filter pushdown apply, and remote
  archives are read with range requests.
- One archive per call. A list fails with
  `No function matches ... 'cozip_offset_size(VARCHAR[])'`; combine archives with
  `UNION ALL BY NAME`. `read_flat(NULL)` fails with
  `read_parquet cannot take NULL list as parameter`.
- The location option set to `false` returns the column filled with NULL; the language
  wrappers drop it.
- GeoParquet geometry is typed `GEOMETRY` with its CRS (DuckDB 1.5), so spatial functions
  apply after `LOAD spatial`.
- From 2.0.2 a stored `cozip:location` or `cozip:gdal_vsi` column is excluded and
  recomputed. The 2.0.1 macro is `SELECT *` plus the computed column, so a stored copy
  passes through in SQL (the language wrappers drop it).

## 4. Recipes

Version-independent projection without the location column:

```sql
SELECT COLUMNS(lambda c: c NOT LIKE 'cozip:%') FROM read_flat('dataset.zip');
```

Summaries and filters:

```sql
SELECT continent, count(*) AS files, sum(size) AS bytes
FROM read_flat('https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip')
GROUP BY continent ORDER BY files DESC;
```

Spatial filter on a GeoParquet manifest:

```sql
INSTALL spatial; LOAD spatial;
SELECT name, "offset", "size"
FROM read_flat('cities.zip')
WHERE ST_Intersects(geometry, ST_MakeEnvelope(-80, -15, -70, 0));
```

Several archives:

```sql
SELECT 'a.zip' AS archive, * FROM read_flat('a.zip')
UNION ALL BY NAME
SELECT 'b.zip' AS archive, * FROM read_flat('b.zip');
```

Build the location yourself (any extension version, any spelling of the archive URL):

```sql
SELECT name,
       '/vsisubfile/' || "offset" || '_' || "size" || ',/vsicurl/' || 'https://example.org/data.zip' AS location
FROM read_flat('https://example.org/data.zip', gdal_vsi := false);   -- location := false on 2.0.2+
```

Export the manifest:

```sql
COPY (SELECT COLUMNS(lambda c: c NOT LIKE 'cozip:%') FROM read_flat('dataset.zip'))
TO 'manifest.parquet' (FORMAT parquet);
```

`offset` is an SQL keyword: `SELECT offset FROM ...` is a syntax error, so always write
`"offset"`. `size` works unquoted.

## 5. Location values

`/vsisubfile/{offset}_{size},{base}`, where the base depends on the path passed in
(measured on 2.0.1):

| Path | Base |
| --- | --- |
| `https://host/a.zip`, `http://...` | `/vsicurl/https://host/a.zip` |
| `s3://bucket/key.zip` | `/vsis3/bucket/key.zip` |
| `gs://bucket/a.zip`, `gcs://bucket/a.zip` | `/vsigs/bucket/a.zip` |
| `azure://container/a.zip` | `/vsiaz/container/a.zip` |
| `abfss://c@account.dfs.core.windows.net/a.zip` | `/vsiadls/c@account.dfs.core.windows.net/a.zip` |
| `hf://datasets/org/repo/path.zip` | `/vsicurl/https://huggingface.co/datasets/org/repo/resolve/main/path.zip` |
| `/abs/a.zip`, `rel/a.zip` | unchanged, so relative stays relative |

Up to 2.0.2 an `hf://` path with a revision is mangled:
`hf://datasets/org/repo@v0.1.0/data/x.zip` becomes
`/vsicurl/https://huggingface.co/datasets/org/repo@v0.1.0/resolve/main/data/x.zip`, which
answers 401. Read from `https://huggingface.co/datasets/org/repo/resolve/v0.1.0/data/x.zip`
instead, or rebuild the location. The fix is on cozip_reader `main`.

GDAL needs its own credentials for `/vsis3/`, `/vsigs/` and `/vsiaz/` (for example
`AWS_*` environment variables); DuckDB secrets do not reach GDAL.

## 6. Remote and private sources

`httpfs` autoloads for `http(s)://`, `s3://`, `gs://` and `hf://`. Private data needs a
DuckDB secret on the same connection, or a persistent one that every new connection
(including the ones `cozip.read` opens) loads from `~/.duckdb/stored_secrets`:

```sql
CREATE SECRET (TYPE s3, KEY_ID '...', SECRET '...', REGION 'us-west-2');
CREATE SECRET hf (TYPE huggingface, TOKEN 'hf_...');
CREATE PERSISTENT SECRET my_bucket (TYPE s3, KEY_ID '...', SECRET '...', SCOPE 's3://my-bucket');
```

## 7. Errors

| Message (excerpt) | Cause |
| --- | --- |
| `Binder Error: Macro read_flat() does not support the supplied arguments ... Candidate macros: read_flat(p, gdal_vsi := ...)` | extension 2.0.0 or 2.0.1; use `gdal_vsi :=` or update |
| `Catalog Error: Table Function with name read_flat does not exist!` | extension not loaded (`LOAD cozip`), or 1.x (update, or use `read_cozip`) |
| `Parser Error: syntax error at or near "FROM"` after `SELECT offset` | quote `"offset"` |
| `read_flat needs a Flat-profile archive (profile=1). Got profile=taco in: ...` | TACO archive (`profile=none` for profile 0) |
| `LFH does not match cozip layout: ...` or `byte 0 is not a ZIP Local File Header: ...` | a plain ZIP, or not a ZIP at all |
| `cozip index has no entry named '__metadata__': ...` | profile byte 1 without a Flat index |
| `cozip integrity hash mismatch: ...` | modified, truncated or re-encoded archive |
| `cozip archive too small (minimum is 32819 bytes): ...` | not a cozip, or an error page |
| `cozip index entry '...' has a range outside the archive: ...` | corrupt index or truncated object |
| `IO Error: Cannot open file "...": No such file or directory` | wrong path; relative paths resolve against DuckDB's working directory |
| `HTTP 403` or `HTTP 401` | credentials, expired presigned URL, mangled `hf://` revision |
