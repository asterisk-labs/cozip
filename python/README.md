# cozip

Python bindings for libcozip. Read a Cloud-Optimized ZIP archive's manifest over HTTP range requests, or write one from a pyarrow Table.

The native `libcozip` binary ships inside the wheel, no C toolchain required.

## Install

```bash
pip install cozip
```

## Usage

### Write

```python
import cozip
import pyarrow as pa

table = pa.table({
    "name": ["a.txt", "b.bin"],
    "path": ["/path/to/a.txt", "/path/to/b.bin"],
})

cozip.write("out.zip", table)
```

`name` is how each file appears inside the archive. `path` is where it lives on disk, used at write time and dropped from the manifest. Any extra columns ride along into `__metadata__` and become queryable on read.

```python
table = pa.table({
    "name":      ["a.tif", "b.tif"],
    "path":      ["/path/to/a.tif", "/path/to/b.tif"],
    "cloud_pct": [12.3, 45.1],
})

cozip.write("out.zip", table)
```

### Read

```python
import cozip

manifest = cozip.read("https://example.com/dataset.zip")
train = manifest[manifest["split"] == "train"]
```

`manifest` is a pandas DataFrame with `name`, `offset`, `size`, `cozip:gdal_vsi`, plus whatever extras the writer added. Local file or remote URL, same call. Only the byte-0 index and the embedded `__metadata__` Parquet are fetched, never the user payloads. Pass `columns=[...]` to bring only specific extras, `gdal_vsi=False` to drop the VSI path column.

## Versioning

The Python package version tracks the C library, copied verbatim from the repo-root `VERSION` file at build time. The bundled `.so` / `.dylib` / `.dll` inside the wheel matches the wheel's version exactly, so there's no mismatch to worry about.

```python
import cozip
cozip.__version__                  # "2026.5.16"
cozip.lib.cozip_version_string()   # full C library version
```

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).