<div align="center">
  <img src="images/banner.svg" alt="cozip — DuckDB extension" width="700"/>

  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-EAB308?style=flat-square" alt="License MIT"/></a>
    <a href="https://duckdb.org/community_extensions/extensions/cozip.html"><img src="https://img.shields.io/badge/duckdb-community--extension-FFF000?logo=duckdb&logoColor=black&style=flat-square" alt="DuckDB community extension"/></a>
    <a href="https://shell.duckdb.org"><img src="https://img.shields.io/badge/try-shell.duckdb.org-654FF0?logo=webassembly&logoColor=white&style=flat-square" alt="Try in browser"/></a>
    <a href="https://github.com/asterisk-labs/cozip/blob/main/SPEC.md"><img src="https://img.shields.io/badge/cozip-format--%26--spec-A8B9CC?style=flat-square" alt="cozip format"/></a>
  </p>
</div>

---

Query a [cozip](https://github.com/asterisk-labs/cozip) archive as a SQL table — locally, over HTTPS, S3, GCS, Azure, or HuggingFace — without downloading it. cozip places a Parquet manifest at byte 0 of the ZIP, so a multi-gigabyte archive becomes a queryable table with one or two HTTP range requests. No central-directory scan, no full download.

The archive is still a valid ZIP. `unzip`, `zipfile.ZipFile`, your OS file preview — all unchanged.

## Install

```sql
INSTALL cozip FROM community;
LOAD cozip;
```

Works on every DuckDB target: Linux, macOS, Windows, and WebAssembly (try it on `shell.duckdb.org`).

## Query

```sql
SELECT *
FROM read_cozip('https://huggingface.co/datasets/Major-TOM/Core-VIIRS-Nighttime-Light/resolve/main/2024/MAJORTOM-VIIRS-NTL_2024_median_000.zip')
LIMIT 10;
```

One row per entry inside the archive: `name`, `offset`, `size`, plus whatever columns the writer included in `__metadata__` (`split`, `label`, `geometry`, …). Filter, join, sample, then fetch only the payloads you actually need.

```sql
-- Sample 32 training tiles from a remote archive without downloading it.
SELECT name, "cozip:gdal_vsi"
FROM read_cozip('s3://my-bucket/dataset.cozip')
WHERE split = 'train'
USING SAMPLE 32 ROWS;
```

## URL schemes

| Scheme              | Backend  | Notes                                              |
|---------------------|----------|----------------------------------------------------|
| `/local/path`       | local FS | No extension required.                             |
| `https://`          | `httpfs` | Autoloads. Supports Range requests.                |
| `s3://`             | `httpfs` | S3-compatible (R2, MinIO) via `SET s3_*` settings. |
| `gs://`, `gcs://`   | `httpfs` | Google Cloud Storage.                              |
| `azure://`          | `azure`  | Autoloads.                                         |
| `hf://datasets/...` | `hf`     | Autoloads.                                         |

## The `cozip:gdal_vsi` column

Every row includes a synthetic `cozip:gdal_vsi` column with a ready-made `/vsisubfile/<offset>_<size>,/vsi.../<url>` path. Hand it to GDAL, rasterio, or anything that speaks VSI to open the inner file without re-downloading the archive.

```python
import duckdb, rasterio

rows = duckdb.sql("""
    SELECT name, "cozip:gdal_vsi"
    FROM read_cozip('https://.../dataset.zip')
    WHERE split = 'val'
    LIMIT 8
""").fetchall()

for name, vsi in rows:
    with rasterio.open(vsi) as src:
        ...   # GDAL issues range requests against the archive
```

Skip it with `gdal_vsi := false`:

```sql
SELECT name, offset, size
FROM read_cozip('dataset.zip', gdal_vsi := false);
```

## See also

- **[cozip](https://github.com/asterisk-labs/cozip)** — the format, the spec, and bindings for Python, R, Julia, JavaScript, and C.

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
