# R API

Everything here is the `cozip` R package 2026.9.9 as implemented in `r/R/cozip.R`,
`r/R/read.R`, `r/R/zzz.R` and `r/src/cozip_glue.c`. Examples were run with R 4.6,
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

- R 4.1 or newer. Imports: arrow (>= 14), bit64, duckdb, DBI, tibble. Suggests sf and
  geoarrow for GeoParquet.
- r-universe builds the package from the `r/` directory of the `main` branch on every
  push, with binaries for Linux, macOS and Windows. A source install compiles libcozip,
  libzip 1.11.4 and zlib 1.3.1 from `r/src/`; it needs a C toolchain and GNU make, no
  system libraries (Windows adds `-ladvapi32`).
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

- `source`: a single non-empty ASCII string, local path or http(s)/s3/gcs/azure/hf URL.
- `columns`: `NULL` for every column, otherwise extra column names; `name`, `offset`,
  `size` (and `cozip:location` when enabled) are always included.
- Returns a tibble. `offset` and `size` are doubles (DuckDB maps `UBIGINT` to double in R),
  exact up to 2^53 bytes.
- Every call creates its own DuckDB database, runs `INSTALL httpfs`, `LOAD httpfs`,
  `INSTALL cozip FROM community` and `LOAD cozip` (or `LOAD` the file named by the
  `COZIP_EXTENSION` environment variable), queries `read_flat()` and shuts the database
  down. The duckdb package prints a note that extensions live under `~/.duckdb`.
- The same fallbacks as Python apply: `gdal_vsi :=` for extension 2.0.0 and 2.0.1,
  `read_cozip()` for 1.x, legacy `cozip:gdal_vsi` renamed to `cozip:location`,
  `taco:location` dropped. The same known issue too: with those extensions a non-NULL
  `columns` loses `cozip:location`; read all columns and subset afterwards.
- Only Flat archives are accepted.

## 6. Errors

- Checks written in R stop with `cozip: ...` and no call, for example
  `cozip: duplicate name 'x' at row 2`,
  `` cozip: `table` must be an arrow::Table (got data.frame) ``,
  `cozip: name mismatch at row 1: parquet='a.txt', paths='data/b.bin'`.
- libcozip failures carry the status name:
  `[cozip:INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must
  be non-empty`. The entry index is zero-based there and one-based in R-side messages.
- DuckDB errors from `read()` pass through, for example
  `Invalid Input Error: read_flat needs a Flat-profile archive (profile=1). Got
  profile=taco ...`.

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
