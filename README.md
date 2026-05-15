<div align="center">
  <img src="images/banner.svg" alt="cozip — Cloud Optimized ZIP" width="700"/>

  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-EAB308?style=flat-square" alt="License MIT"/></a>
    <a href="https://pypi.org/project/cozip"><img src="https://img.shields.io/pypi/v/cozip?label=python&logo=python&logoColor=white&color=3776AB&style=flat-square" alt="PyPI"/></a>
    <a href="https://asterisk-labs.r-universe.dev/cozip"><img src="https://img.shields.io/badge/r--universe-cozip-276DC3?logo=r&logoColor=white&style=flat-square" alt="R"/></a>
    <a href="https://juliahub.com/ui/Packages/Cozip"><img src="https://img.shields.io/badge/julia-1.10%2B-9558B2?logo=julia&logoColor=white&style=flat-square" alt="Julia"/></a>
    <a href="https://www.npmjs.com/package/cozip"><img src="https://img.shields.io/npm/v/cozip?label=npm&logo=npm&logoColor=white&color=CB3837&style=flat-square" alt="npm"/></a>
    <a href="#wasm"><img src="https://img.shields.io/badge/wasm-browser--ready-654FF0?logo=webassembly&logoColor=white&style=flat-square" alt="WASM"/></a>
    <a href="https://duckdb.org/community_extensions"><img src="https://img.shields.io/badge/duckdb-extension-FFF000?logo=duckdb&logoColor=black&style=flat-square" alt="DuckDB"/></a>
    <a href="#core"><img src="https://img.shields.io/badge/C11-core-A8B9CC?logo=c&logoColor=white&style=flat-square" alt="C11"/></a>
  </p>
</div>

---

## What is cozip?

A ZIP file you can open like a table — over the network, without downloading it.

cozip puts a Parquet manifest called `__metadata__` at **byte 0** — one row per entry with name, offset, size, plus any columns you add (`split`, `label`, `class`...). DuckDB, Arrow, and Polars query it directly. Range requests fetch only the bytes you actually need.

A 20 GB archive becomes a queryable table.

**It's still a ZIP.** `unzip`, `zipfile.ZipFile`, your OS's preview window — all unchanged.

## Install

```bash
pip install cozip
```

## Usage

Two functions: `write` and `read`.

### Write

```python
import cozip
import polars as pl

df = pl.DataFrame({
    "path":  ["local/tile_001.tif", "local/tile_002.tif", "local/tile_003.tif"],
    "name":  ["tile_001.tif", "tile_002.tif", "tile_003.tif"],
    "split": ["train", "val", "train"],
    "label": ["cloud", "water", "forest"],
})

cozip.write("dataset.zip", df)
```

Two reserved columns. `path` is where the file lives on disk — it's consumed at write time and dropped. `name` is how the entry is stored inside the archive and becomes part of `__metadata__`. Every other column rides along and becomes queryable on read.

### Read

```python
df = cozip.read("dataset.zip")
```

Local file or remote URL — same call. You get a DataFrame back with one row per entry, including `offset` and `size` resolved against the archive.

```python
df = cozip.read("https://example.com/dataset.zip")

# query the manifest like any DataFrame
batch = df.filter(pl.col("split") == "train").sample(32)

# batch.select(["name", "offset", "size"]) is everything you need
# to range-request the payloads
```

## Bindings

| Language     | Install                                                                       |
|--------------|-------------------------------------------------------------------------------|
| Python       | `pip install cozip`                                                           |
| R            | `install.packages("cozip", repos = "https://asterisk-labs.r-universe.dev")`   |
| Julia        | `Pkg.add("Cozip")`                                                            |
| JavaScript   | `npm install cozip`                                                           |
| WASM         | browser bundle, no Node required                                              |
| DuckDB       | `INSTALL cozip FROM community; LOAD cozip;`                                   |
| C            | vendored single-header `cozip.h`                                              |

All bindings call into the same C11 core. Byte-exact behavior across runtimes.

## Specification

The on-disk format is defined in [SPEC.md](cozip/SPEC.md). Any conforming implementation reads any cozip ever written.

## License

MIT

<div align="center">
  <br>
  Developed with ❤️ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="images/asterisk_logo.svg" alt="Asterisk Labs" width="400"/>
  </a>
</div>