# Opening entries and publishing archives

Sources: `SPEC.md` 13.4 and Appendix A, `docs/python.html`, the public fixtures in
`asterisk-labs/cozip-api-fixtures`. Every snippet ran against a local archive or the
fixture at `https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip`
(also served from `https://data.source.coop/asterisk-labs/cozip-api-fixtures/data/cities.zip`).

## Contents

1. What a manifest row addresses
2. GDAL, rasterio and terra
3. Plain byte ranges
4. Many files: dataloaders and contiguous reads
5. Publishing
6. HTTP requirements

## 1. What a manifest row addresses

`offset` is the first byte of the file's payload, counted from byte 0 of the archive, and
`size` is its length. Entries are stored uncompressed, so bytes `offset` to
`offset + size - 1` are the original file, unchanged. HTTP ranges are inclusive:
`Range: bytes={offset}-{offset + size - 1}`. Nothing between the manifest and those bytes
needs decoding, and the Central Directory is never read.

## 2. GDAL, rasterio and terra

`cozip:location` is a GDAL virtual path: `/vsisubfile/{offset}_{size},{archive}`, with
`/vsicurl/` in front of HTTP(S) archives. Any GDAL-based reader opens it without
extracting anything:

```python
import cozip, rasterio

manifest = cozip.read("/abs/path/tiles.zip")
with rasterio.open(manifest["cozip:location"].iloc[0]) as src:
    band = src.read(1)
```

```bash
ogrinfo -ro -so -al "/vsisubfile/141_181,/vsicurl/https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip"
```

```r
m <- cozip::read(normalizePath("tiles.zip"))
r <- terra::rast(m[["cozip:location"]][1])
```

- The archive part repeats what was passed to `read()`. Read with an absolute path or a
  URL when the location will be used from another directory or machine.
- Build it yourself when the reader did not emit it:
  `f"/vsisubfile/{offset}_{size},/vsicurl/{url}"` for HTTP(S), or
  `f"/vsisubfile/{offset}_{size},{absolute_path}"` for a local file.
- For `s3://`, `gs://` and Azure archives the location uses `/vsis3/`, `/vsigs/`,
  `/vsiaz/`; GDAL then needs its own credentials. `hf://` paths with a revision produce a
  broken location on extension 2.0.x (`duckdb-reader.md` section 5); use the HTTPS
  `resolve/<revision>` URL.

## 3. Plain byte ranges

Local file:

```python
with open(archive, "rb") as f:
    f.seek(offset)
    data = f.read(size)
```

`requests`:

```python
import requests
r = requests.get(url, headers={"Range": f"bytes={offset}-{offset + size - 1}"})
assert r.status_code == 206
data = r.content
```

`obstore` (one or many ranges of one object):

```python
import obstore
from obstore.store import HTTPStore

store = HTTPStore.from_url("https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data")
data = bytes(obstore.get_range(store, "cities.zip", start=141, length=181))
parts = obstore.get_ranges(store, "cities.zip", starts=[141, 372], lengths=[181, 181])
```

`curl`:

```bash
curl -L -r 141-321 https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip
```

JavaScript uses `fetch` with the same header; keep the arithmetic in `BigInt`
(`javascript-api.md` section 5). `urllib.request` with its default `User-Agent` gets
`403 Forbidden` from Source Cooperative; send any other `User-Agent`.

## 4. Many files: dataloaders and contiguous reads

Read the manifest once, keep plain integer arrays, and open connections inside each
worker. A dataset-style wrapper (the framework is up to you):

```python
import cozip
import obstore
from obstore.store import HTTPStore

class CozipItems:
    def __init__(self, base, archive, **filters):
        manifest = cozip.read(f"{base}/{archive}", location=False)
        for column, value in filters.items():
            manifest = manifest[manifest[column] == value]
        self.names = manifest["name"].tolist()
        self.offsets = manifest["offset"].astype("int64").tolist()
        self.sizes = manifest["size"].astype("int64").tolist()
        self.base, self.archive, self.store = base, archive, None

    def __len__(self):
        return len(self.names)

    def __getitem__(self, i):
        if self.store is None:        # created lazily, so each worker process gets its own
            self.store = HTTPStore.from_url(self.base)
        data = obstore.get_range(self.store, self.archive, start=self.offsets[i], length=self.sizes[i])
        return self.names[i], bytes(data)
```

Neighbouring entries can come back in one request: fetch from the first offset to the end
of the last payload and slice. The bytes between payloads are local headers
(30 bytes plus the name, plus 20 for ZIP64 entries), which you simply skip:

```python
# manifest from cozip.read, store from HTTPStore.from_url as in section 3
run = manifest.sort_values("offset").iloc[0:4]
start = int(run["offset"].iloc[0])
end = int(run["offset"].iloc[-1] + run["size"].iloc[-1])
blob = bytes(obstore.get_range(store, "cities.zip", start=start, end=end))
parts = [blob[int(o) - start:int(o) - start + int(s)] for o, s in zip(run["offset"], run["size"])]
```

Writers control adjacency: entries land in table order, so sort the input table by the
key you will read by (split, tile, time) before `stage_metadata`.

## 5. Publishing

- The archive is a single immutable object; upload it with any tool. `docs/python.html`
  shows `boto3.upload_file` to Source Cooperative's S3 bucket and reading it back from
  `https://data.source.coop/...`. Hugging Face datasets serve it from
  `https://huggingface.co/datasets/<org>/<repo>/resolve/<revision>/<path>`.
- Never modify or replace an archive in place. Readers may hold its manifest, and every
  offset changes when anything is rebuilt. Publish a new key or a new revision; pin a
  revision (tag or commit) in URLs used by tests and training runs.
- Keep the `.zip` extension and `application/zip`. Detection relies on the bytes, not the
  name.
- A large collection can also be split into several archives, as the TACO writer's
  `partition_size` option does. Each keeps its own manifest and can be rebuilt alone;
  DuckDB combines the manifests with `UNION ALL BY NAME`.
- Once public, the playground (`playground.html`) renders a manifest from its URL, and
  a GeoParquet manifest on a map.

## 6. HTTP requirements

| Requirement | Why |
| --- | --- |
| `Range` requests answered with `206` and `Content-Range: bytes a-b/total` | readers fetch a few small ranges (index, tail, manifest) instead of the archive; the JavaScript reader takes the archive size from `Content-Range` |
| The stored bytes, with no `Content-Encoding` (gzip or brotli) applied on the fly | offsets, the integrity hash and ZIP sizes refer to the stored bytes (spec Appendix A) |
| CORS for browsers: allow the `Range` request header, expose `Content-Range` | the JavaScript reader reads `Content-Range` from every partial response |
| A stable object during a read | the JavaScript reader fails when the size changes between requests; others may read mixed versions |

A server that ignores `Range` and returns the full object with `200` still works for the
JavaScript reader, but every read downloads the whole archive.
