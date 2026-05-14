# Cozip.jl

Julia binding for [libcozip](https://github.com/asterisk-labs/taco/tree/main/cozip) — pack files into a Cloud-Optimized ZIP archive readable over HTTP range requests.

The native `libcozip` binary is fetched automatically via Julia Artifacts; no C toolchain required.

## Install

From the AsteriskRegistry (recommended):

```julia
using Pkg
Pkg.Registry.add(RegistrySpec(url = "https://github.com/asterisk-labs/AsteriskRegistry"))
Pkg.add("Cozip")
```

Or directly from the monorepo:

```julia
Pkg.add(url = "https://github.com/asterisk-labs/taco", subdir = "cozip/julia")
```

## Usage

### Simple mode

```julia
using Cozip, DataFrames

table = DataFrame(
    name = ["a.txt", "b.bin"],
    path = ["/path/to/a.txt", "/path/to/b.bin"],
)

Cozip.create("out.cozip", table)
```

### Two-step mode

For inspecting or tuning the `__metadata__` parquet between steps:

```julia
# 1) materialize the __metadata__ parquet
Cozip.metadata("meta.parquet", table; create_options = "COMPRESSION 'zstd'")

# 2) pack the archive
Cozip.create("out.cozip", "meta.parquet")
```

`create_options` is passed through to DuckDB's `COPY ... TO '...' (FORMAT parquet, <opts>)`, so anything DuckDB accepts works (e.g. `"COMPRESSION 'zstd', ROW_GROUP_SIZE 100000"`).

### Extra columns

Any additional columns in `table` are propagated into `__metadata__`:

```julia
table = DataFrame(
    name      = ["a.tif", "b.tif"],
    path      = ["/path/to/a.tif", "/path/to/b.tif"],
    cloud_pct = [12.3, 45.1],
)

Cozip.create("out.cozip", table)
```

The optional `in_index` column (default `true`) controls whether each entry is recorded in the cozip index — entries with `in_index = false` go into the ZIP but aren't exposed as cozip-indexed entries.

## Versioning

`Cozip.jl` tracks the C library, which uses CalVer with 4 components (e.g. `2026.5.2.6`). The Julia package itself uses 3 components because Julia enforces strict SemVer; the fourth is exposed via:

```julia
using Cozip
Cozip.LibCozip.cozip_version()  # → "2026.5.2.6"
```

## Spec

See the [cozip spec](https://github.com/asterisk-labs/taco/tree/main/cozip) for the on-disk format.

## License

MIT — see [LICENSE](../LICENSE).