# Julia API

Everything here is `Cozip.jl` 2026.11.23 as implemented in `julia/src/Cozip.jl`,
`LibCozip.jl`, `Writer.jl` and `Reader.jl`. Examples were run with Julia 1.12,
DataFrames 1 and DuckDB.jl on macOS.

## Contents

1. Install and native library
2. Names and exports
3. `Cozip.write` and `create`
4. `stage_metadata`
5. `stage_create`
6. `Cozip.read`
7. Errors
8. Patterns

## 1. Install and native library

```julia
using Pkg
Pkg.Registry.add(Pkg.RegistrySpec(url = "https://github.com/asterisk-labs/AsteriskRegistry"))
Pkg.add("Cozip")
```

- Julia 1.10 or newer. Dependencies: DataFrames, DuckDB, DBInterface.
- libcozip is a lazy artifact (`julia/Artifacts.toml`), downloaded from the matching
  GitHub release on first load. Platforms: Linux x86-64 and aarch64 (glibc), macOS
  aarch64 and x86-64, Windows x86-64. The macOS libraries include the reader and
  link libcurl and OpenSSL statically.
- `COZIP_LIB_PATH=/abs/path/cozip.dylib` overrides the artifact. It is read in
  `LibCozip.__init__`, so set it before `using Cozip`.
- `Cozip.LibCozip.cozip_version()` returns the loaded library's version.

## 2. Names and exports

- Exported: `create`, `stage_metadata`, `stage_create`.
- Not exported: `Cozip.write` (`const write = create`), `Cozip.read` and `Cozip.verify`. After
  `using Cozip`, a bare `write` or `read` is still `Base.write` or `Base.read`; qualify them.

## 3. `Cozip.write` and `create`

```julia
Cozip.write(out_path, table; temp_dir = nothing) -> String
```

- `table` is any Tables.jl source with `name` and `path` columns; it is copied into a
  `DataFrame`. Other columns go into the manifest.
- The table is planned once. The temporary manifest is the DataFrame registered with
  DuckDB (`register_data_frame`) and written with `COPY ... (FORMAT parquet, COMPRESSION zstd)`, as Parquet version 1 or 2
  depending on which writes the first 10,000 rows smaller, into
  `temp_dir` (default `tempdir()`): `UInt64` becomes `UBIGINT`, `Vector{UInt8}` `BLOB`
  (scanned as hex and decoded with `unhex()`, since DuckDB.jl before 1.5.5 cannot scan
  a `Vector{UInt8}` column),
  integers, floats (NaN and Inf included), `Bool`, `String`, `Date` and `DateTime` keep
  their types, `missing` becomes NULL, and an element type DuckDB.jl cannot store, such as
  `Vector{Int}`, raises `ArgumentError` instead of being stringified. Table-level
  `DataFrames.metadata` is stored as Parquet key-value metadata, so a `geo` entry survives.
- The archive is written with the planned sizes; the C writer stops when a source changed
  size since planning.
- Column names are compared without case, as DuckDB compares them, and `name` values must
  be strings.
- Returns `abspath(out_path)`.

## 4. `stage_metadata`

```julia
metadata, paths = stage_metadata(table)
# metadata::DataFrame  name::String, offset::UInt64, size::UInt64, extras...
# paths::Vector{Tuple{String,String}}  row-aligned (name, path)
```

- Checks: `name` and `path` present; no `offset`, `size`, `cozip:location` or
  `taco:location`; at least one row; no `missing` or `nothing`; no NUL; no reserved name;
  no duplicate; every path satisfies `isfile` (a directory is reported as
  `source not found`).
- `cozip_plan_flat` in C then enforces name syntax, ASCII and non-empty files.
- Table-level metadata of the input is copied to `metadata`.

## 5. `stage_create`

```julia
stage_create(out_path, paths, metadata_parquet; validate = true) -> String
```

- `paths` is an iterable of two-element `Tuple`s `(name, source_path)` in manifest row
  order; vectors of vectors are rejected.
- The Parquet must exist and be non-empty. Its columns are read through the package's
  shared DuckDB connection: `name`, `offset`, `size` required, `path` and location columns
  forbidden.
- `validate = true` compares names, offsets and sizes with a new plan and reports the first
  mismatching row (one-based).
- Writes with `cozip_write_flat`; returns `abspath(out_path)`.

## 6. `Cozip.read`

```julia
Cozip.read(source; columns = nothing, location = true) -> DataFrame
```

- `source`: non-empty ASCII path or URL without NUL. `columns`: `nothing` or a vector of
  extra column names. `offset` and `size` come back as `UInt64`.
- `Cozip.LibCozip.read_flat_metadata(source)` verifies the index and returns the
  `__metadata__` bytes; `Cozip.LibCozip.location_base(source)` returns the GDAL base of
  the location. Schemes and credentials are those of Python (`python-api.md` sections 5
  and 8).
- The manifest is written to a `mktempdir()` and decoded on the one DuckDB connection
  created in `Cozip.__init__`, guarded by `Cozip._DUCKDB_LOCK`. That connection, shared
  with the writer, has `autoinstall_known_extensions` and `autoload_known_extensions`
  off, so no extension is ever installed or loaded. `COZIP_EXTENSION` is ignored.
- Stored location columns are dropped and `cozip:location` is computed again, as in
  Python. Hive partitioning is off. Only Flat archives are accepted.
- On Windows `Cozip.read` throws an `ErrorException` before reading, because the native
  reader has not been verified there from Julia yet. Writing works on Windows; read with Python or R
  instead.
- `Cozip.verify(source) -> nothing` checks the access path of any profile without decoding its
  manifest, as `cozip.verify` does (`python-api.md` section 5), through
  `Cozip.LibCozip.verify_archive`. It throws `CozipError` named after the broken rule,
  shares `_check_source` with `read`, and has the same Windows guard.

## 7. Errors

| Type | Example |
| --- | --- |
| `ArgumentError` | `cozip: duplicate name "x" at row 2` |
| `ArgumentError` | `cozip: row 1 ("d"): source is not a regular file: .` |
| `Cozip.LibCozip.CozipError` | `CozipError [INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must be non-empty` |

`CozipError` has fields `code`, `name` and `message`. `Cozip.read` raises it for index and
transport failures, for example `CozipError [PROFILE] read_flat needs a Flat-profile archive
(profile=1). Got profile=2 in: ...`. An unknown `columns` entry, matched exactly, or
manifest columns that differ only in case raise `ArgumentError`.

## 8. Patterns

### Round trip

```julia
using Cozip, DataFrames

tbl = DataFrame(name = ["a.txt", "data/b.bin"], path = ["a.txt", "b.bin"], split = ["train", "val"])
out = Cozip.write("out.zip", tbl)
m = Cozip.read(out; columns = ["split"])
open(out) do io
    seek(io, m.offset[1])
    read(io, m.size[1])          # the bytes of a.txt
end
```

### Custom manifest

Write the Parquet with any tool, keeping `offset` and `size` as unsigned 64-bit, then pass
it to `stage_create` with the `paths` returned by `stage_metadata`. The package tests
(`julia/test/runtests.jl`, GeoParquet round trip) do it with GeoParquet.jl:

```julia
using GeoParquet
import GeoInterface as GI

md, paths = stage_metadata(tbl)
md.geometry = [GI.Point((-77.04, -12.05)), GI.Point((2.35, 48.86))]
GeoParquet.write("meta.parquet", md, (:geometry,))
stage_create("out.zip", paths, "meta.parquet")
```
