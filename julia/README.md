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

Archive names must be ASCII. Writer source and output paths may contain
Unicode; paths and URLs passed to `Cozip.read()` are currently ASCII-only.
Empty files are not valid cozip entries.

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

`manifest` is a DataFrame: `name`, `offset`, `size`, `cozip:location`, and the writer's extras. Local path or URL, same call. It fetches the byte-0 index and `__metadata__`, never the payloads.

`columns = [...]` picks extras. `location = false` drops `cozip:location`.

`Cozip.read()` supports only Flat-profile archives (`profile = 1`). TACO
archives (`profile = 2`) and all other profiles are rejected.

## Versioning

`Cozip.jl` tracks the C library. `make sync` keeps `Project.toml` aligned with
the repository's CalVer version.

```julia
using Cozip
Cozip.LibCozip.cozip_version()  # "2026.9.4"
```

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).
