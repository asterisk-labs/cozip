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

Archive names must be ASCII. Writer source and output paths may contain
Unicode; paths and URLs passed to `cozip::read()` are currently ASCII-only.
Empty files are not valid cozip entries.

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

`manifest` is a tibble: `name`, `offset`, `size`, `cozip:location`, and the writer's extras. Local path or URL, same call. It fetches the byte-0 index and `__metadata__`, never the payloads.

`columns = c(...)` picks extras. `location = FALSE` drops `cozip:location`.

`cozip::read()` supports only Flat-profile archives (`profile = 1`). TACO
archives (`profile = 2`) and all other profiles are rejected.

## Versioning

The R package version tracks the C library exactly and is copied from the
repo-root `VERSION` file by `make sync`.

```r
packageVersion("cozip")  # "2026.9.4"
```

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).
