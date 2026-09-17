# Julia API

Everything here is `Cozip.jl` 2026.9.9 as implemented in `julia/src/Cozip.jl`,
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

- Julia 1.10 or newer. Dependencies: DataFrames, DuckDB, DBInterface, Tables.
- libcozip is a lazy artifact (`julia/Artifacts.toml`): the matching
  `libcozip-<version>-<platform>.tar.gz` from the GitHub release downloads on first load.
  Platforms: Linux x86-64 and aarch64 (glibc), macOS universal, Windows x86-64.
- `COZIP_LIB_PATH=/abs/path/cozip.dylib` overrides the artifact. It is read in
  `LibCozip.__init__`, so set it before `using Cozip`.
- `Cozip.LibCozip.cozip_version()` returns the loaded library's version.

## 2. Names and exports

- Exported: `create`, `stage_metadata`, `stage_create`.
- Not exported: `Cozip.write` (`const write = create`) and `Cozip.read`. After
  `using Cozip`, a bare `write` or `read` is still `Base.write` or `Base.read`; qualify them.

## 3. `Cozip.write` and `create`

```julia
Cozip.write(out_path, table; temp_dir = nothing) -> String
```

- `table` is any Tables.jl source with `name` and `path` columns; it is copied into a
  `DataFrame`. Other columns go into the manifest.
- The temporary manifest is written by DuckDB (`COPY ... (FORMAT parquet)`) into
  `temp_dir` (default `tempdir()`) with explicit types: `UInt64` becomes `UBIGINT`,
  `Vector{UInt8}` becomes `BLOB`, `Int*`, `UInt*`, `Float*`, `Bool`, `String`, `Date` and
  `DateTime` map to their DuckDB types, `missing` becomes NULL, and any other element type
  is written as `VARCHAR` through `string`. Table-level `DataFrames.metadata` is stored as
  Parquet key-value metadata, so a `geo` entry survives.
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
- Uses one DuckDB connection created in `Cozip.__init__` and guarded by a lock. The first
  read of a session runs `INSTALL httpfs`, `LOAD httpfs`, `INSTALL cozip FROM community` and
  `LOAD cozip`; later reads reuse them.
- `COZIP_EXTENSION` loads a local extension instead. The connection only allows unsigned
  extensions when the variable was set before `using Cozip`.
- Same compatibility fallbacks as Python (`gdal_vsi :=`, `read_cozip`, renamed legacy
  column, dropped `taco:location`). Only Flat archives are accepted.
- Same known issue as Python: with extension 2.0.0 or 2.0.1, passing `columns` drops
  `cozip:location`. Read every column and `select` afterwards when you need the location.
- On Windows `Cozip.read` throws an `ErrorException` before touching DuckDB: DuckDB.jl
  crashes there when a community extension registers a filesystem. Writing works on
  Windows; read with Python or R instead.

## 7. Errors

| Type | Example |
| --- | --- |
| `ArgumentError` | `cozip: duplicate name "x" at row 2` |
| `SystemError` | `cozip: row 1 ("d"): source not found: .` |
| `Cozip.LibCozip.CozipError` | `CozipError [INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must be non-empty` |

`CozipError` has fields `code`, `name` and `message`. DuckDB errors from `Cozip.read` pass
through.

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
