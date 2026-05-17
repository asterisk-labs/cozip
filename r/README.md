# cozip

R bindings for libcozip. Read a Cloud-Optimized ZIP archive's manifest over HTTP range requests, or write one from an arrow Table.

`libzip` and `zlib` are vendored under `src/`, so installation compiles everything from source. No system dependencies beyond a working C toolchain.

## Install

```r
install.packages("cozip", repos = "https://asterisk-labs.r-universe.dev")
```

## Usage

### Write

```r
library(cozip)
library(arrow)

tbl <- arrow_table(
  name = c("a.txt", "b.bin"),
  path = c("/path/to/a.txt", "/path/to/b.bin")
)

create("out.zip", tbl)
```

`name` is how each file appears inside the archive. `path` is where it lives on disk, used at write time and dropped from the manifest. Any extra columns ride along into `__metadata__` and become queryable on read.

```r
tbl <- arrow_table(
  name      = c("a.tif", "b.tif"),
  path      = c("/path/to/a.tif", "/path/to/b.tif"),
  cloud_pct = c(12.3, 45.1)
)

create("out.zip", tbl)
```

### Read

```r
library(cozip)

manifest <- read("https://example.com/dataset.zip")
train <- manifest[manifest$split == "train", ]
```

`manifest` is a tibble with `name`, `offset`, `size`, `cozip:gdal_vsi`, plus whatever extras the writer added. Local file or remote URL, same call. Only the byte-0 index and the embedded `__metadata__` Parquet are fetched, never the user payloads. Pass `columns = c(...)` to bring only specific extras, `gdal_vsi = FALSE` to drop the VSI path column.

## Versioning

The R package version tracks the C library exactly, copied verbatim from the repo-root `VERSION` file by the `make sync` target. The 4-component CalVer (e.g., `2026.5.2.6`) goes into `DESCRIPTION` as-is, no stripping required.

```r
packageVersion("cozip")  # "2026.5.2.6"
```

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).