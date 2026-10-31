# R API

Everything here is the `cozip` R package 2026.9.27 as implemented in `r/R/cozip.R`,
`r/R/read.R`, `r/R/zzz.R`, `r/src/cozip_glue.c` and the native reader
`r/src/cozip_read.c`. Examples were run with R 4.6,
arrow 24, duckdb 1.5.5, sf 1.1, geoarrow 0.4 and terra 1.9 in a UTF-8 locale.

## Contents

1. Install and runtime
2. `create()` and `write()`
3. `stage_metadata()`
4. `stage_create()`
5. `read()`
6. Errors
7. Locale and encodings
8. Patterns

## 1. Install and runtime

```r
install.packages("cozip", repos = "https://asterisk-labs.r-universe.dev")
```

- R 4.3 or newer. Imports: arrow (>= 14), bit64, duckdb, DBI, tibble. Suggests sf and
  geoarrow for GeoParquet.
- r-universe builds the package from the `r/` directory of the `main` branch on every
  push, with binaries for Linux, macOS and Windows. A source install compiles libcozip,
  the reader, libzip 1.11.4, zlib 1.3.1 and karu 0.4.0 from `r/src/`. It needs GNU make,
  a C++23 compiler (`CXX_STD = CXX23`), pkg-config, libcurl 7.83 or newer and OpenSSL 3
  (`libcurl4-openssl-dev` and `libssl-dev` on Debian and Ubuntu; Rtools ships them on
  Windows, which also links `-ladvapi32` and `-lws2_32`).
- `configure` and `configure.win` find libcurl and OpenSSL with pkg-config and write
  `src/Makevars` and `src/Makevars.win` from their `.in` templates; `cleanup` removes
  them. An Emscripten (webR) build leaves the reader out, and `read()` then fails with
  `UNSUPPORTED`.
- Exports: `create`, `write` (the same function), `stage_metadata`, `stage_create`, `read`.
  `library(cozip)` masks `base::write`; call `cozip::write` or `base::write` explicitly
  when both are needed.
- The version tracks the repository: `packageVersion("cozip")`.

## 2. `create()` and `write()`

```r
create(out_path, table, temp_dir = NULL)
```

- `table` must be an `arrow::Table` with `name` and `path` columns; convert a data frame
  with `arrow::as_arrow_table(df)`. Other columns go into the manifest.
- Runs `stage_metadata()`, writes the manifest with `arrow::write_parquet()` defaults to a
  temporary file in `temp_dir` (default `tempdir()`), calls
  `stage_create(validate = FALSE)` and deletes the temporary file.
- Returns `normalizePath(out_path, mustWork = FALSE)`, computed before writing. For a new
  file this is the path as given (still relative); for an existing file it is absolute.

## 3. `stage_metadata()`

```r
res <- stage_metadata(table)
res$metadata   # arrow Table: name, offset <uint64>, size <uint64>, extras
res$paths      # data.frame(name, path), row-aligned with res$metadata
```

- Validates in R: arrow Table, required and reserved columns, no rows, NA values, reserved
  names (read from libcozip at load time), duplicates, missing files, non-regular files.
- Sizes come from `file.size()`; offsets from `cozip_plan_flat()` in C, which also
  enforces name syntax, ASCII and non-empty files.
- Schema metadata on `table` is copied to `metadata`.

## 4. `stage_create()`

```r
stage_create(out_path, paths, metadata_parquet, validate = TRUE)
```

- `paths` is a data frame with character `name` and `path` columns and no NA, in manifest
  row order. `validate` must be a single `TRUE` or `FALSE`.
- The Parquet must exist, be a non-empty regular file, contain `name`, `offset`, `size`
  and contain neither `path` nor a location column. That check always runs.
- `validate = TRUE` compares names, offsets and sizes with a new plan (as doubles, so
  int64 columns also pass).
- Writes through `cozip_finalize()` with the Flat profile. The Parquet is embedded
  verbatim, so compression and schema metadata written by arrow, sf or geoarrow survive.
- Returns `normalizePath(out_path, mustWork = FALSE)`, as `create()` does.

## 5. `read()`

```r
read(source, columns = NULL, location = TRUE)
```

- `source`: a single non-empty ASCII string, local path or URL; the accepted schemes and
  credentials are those of Python (`python-api.md` sections 5 and 8).
- `columns`: `NULL` for every column, otherwise extra column names; `name`, `offset`,
  `size` (and `cozip:location` when enabled) are always included.
- Returns a tibble. `offset` and `size` are doubles (DuckDB maps `UBIGINT` to double in R),
  exact up to 2^53 bytes.
- `.Call(C_R_cozip_read_flat_metadata, source)` verifies the index and returns the
  `__metadata__` bytes, which `read()` writes to a `tempfile()`. A new DuckDB database
  decodes it with `autoinstall_known_extensions` and `autoload_known_extensions` off, plus
  `shared_home = FALSE` and `allow_extensions = FALSE` when the installed duckdb has those
  arguments (1.5.5 does), so nothing touches `~/.duckdb` and no note is printed. The
  database is disconnected before the temporary file is removed.
- No extension is installed or loaded, not even `httpfs`. `COZIP_EXTENSION` is ignored.
- Stored location columns are dropped and `cozip:location` is computed again, as in
  Python. Hive partitioning is off.
- Only Flat archives are accepted.

## 6. Errors

- Checks written in R stop with `cozip: ...` and no call, for example
  `cozip: duplicate name 'x' at row 2`,
  `` cozip: `table` must be an arrow::Table (got data.frame) ``,
  `cozip: name mismatch at row 1: parquet='a.txt', paths='data/b.bin'`.
- libcozip failures carry the status name:
  `[cozip:INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must
  be non-empty`. The entry index is zero-based there and one-based in R-side messages.
- Reader failures also carry the status name, for example
  `[cozip:PROFILE] read_flat needs a Flat-profile archive (profile=1). Got profile=2 in:
  ...` or `[cozip:IO] /path/missing.zip: No such file or directory`. DuckDB errors pass
  through only for an unknown `columns` entry.

## 7. Locale and encodings

Run R in a UTF-8 locale when names or paths may contain non-ASCII characters. Under
`LC_ALL=C` R translates such strings to escape text before they reach C: the name `niño`
becomes the ASCII string `ni<c3><b1>o`, passes the ASCII rule and is written into the
archive under that name, and `read("niño.zip")` looks for a file literally named
`ni<c3><b1>o.zip`. In `en_US.UTF-8` the same name is rejected with
`[cozip:INVALID_ARGUMENT] entry 0 archive name contains non-ASCII bytes; cozip archive
names must be ASCII`. Check with `Sys.getlocale("LC_CTYPE")`.

## 8. Patterns

### GeoParquet manifest with sf and geoarrow

```r
library(geoarrow)                    # registers the sf to GeoArrow conversion
res <- cozip::stage_metadata(tbl)
md <- sf::st_sf(as.data.frame(res$metadata), geometry = geometries)   # an sfc with a CRS
arrow::write_parquet(md, "metadata.parquet")
cozip::stage_create("dataset.zip", res$paths, "metadata.parquet")
```

geoarrow writes native GeoArrow encodings, so `read()` returns a point geometry as a data
frame column with `x` and `y`. To keep sf objects, read the embedded Parquet directly (the
R tests extract `__metadata__` and call `sf::st_as_sfc()` on the geometry column).

### Opening entries

```r
m <- cozip::read(normalizePath("dataset.zip"))
r <- terra::rast(m[["cozip:location"]][1])
```

Use an absolute path or a URL: the location repeats `source` as given.
