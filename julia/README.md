# Cozip.jl

Julia binding for libcozip. Open a Cloud-Optimized ZIP archive like a table over HTTP range requests, or write one from a DataFrame.

The native `libcozip` binary is fetched automatically via Julia Artifacts, no C toolchain required.

## Install

`Cozip.jl` lives in the AsteriskRegistry.

```julia
using Pkg
Pkg.Registry.add(Pkg.RegistrySpec(url="https://github.com/asterisk-labs/AsteriskRegistry"))
Pkg.add("Cozip")
```

## Usage

### Write

```julia
using Cozip, DataFrames

table = DataFrame(
    name = ["a.txt", "b.bin"],
    path = ["/path/to/a.txt", "/path/to/b.bin"],
)

Cozip.write("out.zip", table)
```

`name` is how each file appears inside the archive. `path` is where it lives on disk, used at write time and dropped from the manifest. Any extra columns ride along into `__metadata__` and become queryable on read.

```julia
table = DataFrame(
    name      = ["a.tif", "b.tif"],
    path      = ["/path/to/a.tif", "/path/to/b.tif"],
    cloud_pct = [12.3, 45.1],
)

Cozip.write("out.zip", table)
```

### Read

```julia
using Cozip, DataFrames

manifest = Cozip.read("https://example.com/dataset.zip")
train = filter(:split => ==("train"), manifest)
```

`manifest` is a DataFrame with `name`, `offset`, `size`, plus whatever extras the writer added. Local file or remote URL, same call. Only the byte-0 index and the embedded `__metadata__` Parquet are fetched, never the user payloads. Filter it like any DataFrame, then use `offset` and `size` to range-request the payloads you actually want.

## Versioning

`Cozip.jl` tracks the C library. The C side uses 4-component CalVer (e.g. `2026.5.2.6`). The Julia side uses the first three because Julia enforces strict SemVer. The fourth component is exposed at runtime.

```julia
using Cozip
Cozip.LibCozip.cozip_version()  # "2026.5.2.6"
```

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).