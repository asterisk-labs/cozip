<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="images/cozip-banner-dark.svg">
    <img src="images/cozip-banner-light.svg" alt="cozip" width="436">
  </picture>
  <p>
    <a href="https://pypi.org/project/cozip"><img src="https://img.shields.io/pypi/v/cozip?label=python&logo=python&logoColor=white&color=3776AB&style=flat-square" alt="Python"/></a>
    <a href="https://asterisk-labs.r-universe.dev/cozip"><img src="https://asterisk-labs.r-universe.dev/badges/cozip" alt="R-universe"/></a>
    <a href="https://github.com/asterisk-labs/AsteriskRegistry"><img src="https://img.shields.io/badge/julia-Cozip.jl-9558B2?logo=julia&logoColor=white&style=flat-square" alt="Julia"/></a>
    <a href="https://www.npmjs.com/package/@asterisk-labs/cozip"><img src="https://img.shields.io/npm/v/%40asterisk-labs%2Fcozip?label=npm&logo=npm&style=flat-square" alt="npm"/></a>
    <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-0078D6?style=flat-square" alt="Linux, macOS and Windows"/>
    <a href="#license"><img src="https://img.shields.io/badge/license-MIT-2b8a3e?style=flat-square" alt="MIT license"/></a>
  </p>
</div>

---

cozip opens a ZIP like a table. It stores a Parquet manifest with one row per file
inside an ordinary ZIP, and a small index at byte 0 that says where the manifest
is, so a reader filters the manifest and fetches only the files it needs.

## Features

- **Still a ZIP**, with every file stored as it is, so `unzip`, `zipfile` and any ZIP
  tool list and extract it.
- **An index at byte 0** that gives the offset and size of the manifest, so readers
  never parse the Central Directory.
- **A Parquet manifest** with `name`, `offset` and `size` for every file, plus any
  columns you add, compressed and ready for DuckDB, pandas or Polars.
- **One range per file**, since each row gives the exact bytes of its file.
  `cozip:location` writes that range in
  [GDAL VSI](https://gdal.org/en/stable/user/virtual_file_systems.html) syntax.
- **Reads from anywhere** through [Karu](https://github.com/asterisk-labs/karu),
  whether the archive sits on a local disk, behind HTTP, in S3, GCS, Azure, Hugging
  Face or Source Cooperative.
- **An integrity hash** over the index and the end of the archive, so a truncated or
  rewritten archive fails before any offset is used.
- **Writing in two stages**, so you can plan the offsets first and write the manifest
  with any Parquet tool, GeoParquet included.
- **A C API** in `cozip.h`, so other languages can bind the same core.

## Bindings

| Language | Install | Role | Docs |
|----------|---------|------|------|
| Python | `pip install cozip` | read and write | [README](python/README.md) |
| R | `install.packages("cozip", repos = "https://asterisk-labs.r-universe.dev")` | read and write | [README](r/README.md) |
| Julia | `Pkg.Registry.add(url="https://github.com/asterisk-labs/AsteriskRegistry"); Pkg.add("Cozip")` | read and write | [README](julia/README.md) |
| JavaScript | `npm install @asterisk-labs/cozip` | read | [README](javascript/README.md) |
| C | [`core/`](core/), with libzip, zlib and Karu bundled | read and write | [API](https://asterisk.coop/cozip/api-c.html) |

Python, R and Julia call the C library in [`core/`](core/), and JavaScript has its own
reader for browsers and Node. The optional
[cozip_reader](https://github.com/asterisk-labs/cozip_reader) DuckDB extension queries
the stored files with SQL. See the [guide](https://asterisk.coop/cozip/), the
[specification](SPEC.md) and the [changelog](CHANGELOG).

## Quick start

```python
import pyarrow as pa
import cozip

table = pa.table({
    "name": ["tiles/a.tif", "tiles/b.tif"],   # names inside the archive
    "path": ["data/a.tif", "data/b.tif"],     # files on disk
    "split": ["train", "val"],                # any other column
})
cozip.write("tiles.zip", table)

manifest = cozip.read("tiles.zip")            # also s3://, https:// and other URLs
train = manifest[manifest["split"] == "train"]
```

## AI agent skill

Install the [cozip skill](https://github.com/asterisk-labs/cozip/blob/main/.claude/skills/cozip/SKILL.md) so coding agents know its format rules, binding APIs and TACO writer contract.

```bash
npx skills add asterisk-labs/cozip
```

## Development

```bash
make lib        # build the C library
make python     # Python suite
make help       # R, Julia, JavaScript, docs and sync checks
```

## License

MIT. See [`LICENSE`](LICENSE).

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/asterisk_logo.svg" alt="Asterisk Labs" width="320"/>
  </a>
</div>
