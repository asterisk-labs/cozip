<div align="center">
  <img src="images/banner.svg" alt="cozip" width="700"/>

  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-EAB308?style=flat-square" alt="License MIT"/></a>
    <a href="https://pypi.org/project/cozip"><img src="https://img.shields.io/pypi/v/cozip?label=python&logo=python&logoColor=white&color=3776AB&style=flat-square" alt="PyPI"/></a>
    <a href="https://asterisk-labs.r-universe.dev/cozip"><img src="https://img.shields.io/badge/r--universe-cozip-276DC3?logo=r&logoColor=white&style=flat-square" alt="R"/></a>
    <a href="https://github.com/asterisk-labs/AsteriskRegistry"><img src="https://img.shields.io/badge/julia-Cozip.jl-9558B2?logo=julia&logoColor=white&style=flat-square" alt="Julia"/></a>
    <a href="SPEC.md"><img src="https://img.shields.io/badge/spec-stable-A8B9CC?style=flat-square" alt="Spec"/></a>
  </p>
</div>

---

Open a ZIP like a table. Still a ZIP, now queryable.

cozip glues a Parquet manifest onto an ordinary ZIP. The manifest has one row per entry (`name`, `offset`, `size`, plus any columns you tag onto it). Fetch the index, fetch the manifest, query it locally, then range-request just the bytes you actually want. A 20 GB archive becomes a queryable dataset in two reads.

<div align="center">
  <img src="images/cozip_animation.svg" alt="how cozip works" width="500"/>
</div>

It works because nothing about the ZIP changes. `unzip` works. `zipfile.ZipFile` works. Your OS preview pane works. The manifest is just the first entry, and any conforming ZIP reader walks right past it.

## Two functions

`write` packs files plus metadata into a cozip. `read` returns the manifest. That is the whole surface area in every binding.

The write manifest reserves two columns. `path` is where each file lives on disk, consumed at write time and dropped from the manifest. `name` is how it is stored inside the archive. `write` then adds two more columns to the manifest, `offset` and `size`, holding the byte offset and length of each file in the ZIP.

Everything else rides along and is queryable on read. Local file or remote URL, same call.

### Python

```python
import cozip
import pyarrow as pa

table = pa.table({
    "path":  ["local/tile_001.tif", "local/tile_002.tif", "local/tile_003.tif"],
    "name":  ["tile_001.tif", "tile_002.tif", "tile_003.tif"],
    "split": ["train", "val", "train"],
    "label": ["cloud", "water", "forest"],
})
cozip.write("dataset.zip", table)

manifest = cozip.read("https://example.com/dataset.zip")
train = manifest.filter(pa.compute.equal(manifest["split"], "train"))
```

### R

```r
library(cozip)
library(arrow)

tbl <- arrow_table(
  path  = c("local/tile_001.tif", "local/tile_002.tif", "local/tile_003.tif"),
  name  = c("tile_001.tif", "tile_002.tif", "tile_003.tif"),
  split = c("train", "val", "train"),
  label = c("cloud", "water", "forest")
)
cozip_write("dataset.zip", tbl)

manifest <- cozip_read("https://example.com/dataset.zip")
train <- manifest |> dplyr::filter(split == "train")
```

### Julia

```julia
using Cozip
using DataFrames

df = DataFrame(
    path  = ["local/tile_001.tif", "local/tile_002.tif", "local/tile_003.tif"],
    name  = ["tile_001.tif", "tile_002.tif", "tile_003.tif"],
    split = ["train", "val", "train"],
    label = ["cloud", "water", "forest"],
)
Cozip.write("dataset.zip", df)

manifest = Cozip.read("https://example.com/dataset.zip")
train = filter(:split => ==("train"), manifest)
```

## Bindings

| Language | Read | Write | Install |
|----------|:----:|:-----:|---------|
| Python   |  ✓   |   ✓   | `pip install cozip` |
| R        |  ✓   |   ✓   | `install.packages("cozip", repos = "https://asterisk-labs.r-universe.dev")` |
| Julia    |  ✓   |   ✓   | `Pkg.Registry.add("https://github.com/asterisk-labs/AsteriskRegistry"); Pkg.add("Cozip")` |

Every binding wraps the same C core, so a cozip written by R reads byte for byte identically in Julia, in Python, in C. The high-level API is uniform across runtimes. Python and R speak Apache Arrow tables; Julia speaks Tables.jl-compatible DataFrames.

## Spec

See [SPEC.md](SPEC.md). The format is short and stable. Any conforming reader handles any conforming writer.

## License

MIT.

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="images/asterisk_logo.svg" alt="Asterisk Labs" width="400"/>
  </a>
</div>