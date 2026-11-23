# cozip

JavaScript reader for Flat cozip archives over HTTP. It fetches the index and
Parquet manifest without scanning the ZIP Central Directory.

## Install

```bash
npm install @asterisk-labs/cozip
```

## Usage

```js
import { read } from "@asterisk-labs/cozip";

const manifest = await read("https://example.com/dataset.zip");
const train = manifest.filter((row) => row.split === "train");
```

`manifest` is an array of rows with `name`, `offset`, `size`, `cozip:location`
and any extra columns.

`columns` selects extra columns by exact name; unknown names raise an error.
`location: false` omits `cozip:location`. Reader-owned location columns are
ignored in `columns` and never read from the manifest.

Other profiles, including TACO, raise `UNKNOWN_PROFILE`.

```js
const manifest = await read(url, {
  columns: ["cloud_pct", "split"],
  location: false,
});
```

## Requests

The reader fetches the first 64 KiB, then the last 32 KiB if needed for the
integrity hash. A manifest in those bytes needs no further request; archives
up to 64 KiB take one request. Otherwise, one more range includes the manifest
and its local header. A server that returns the whole archive with `200` also
takes one request.

Later requests use a strong `ETag` as `If-Match` when available. Without one,
size and header checks detect some changes, but cannot guarantee that the
object stayed the same throughout the read.

## Errors

Malformed archives raise errors with a `code` such as `HASH_MISMATCH` or
`UNKNOWN_PROFILE`. The codes follow SPEC.md section 9; `INVALID_INDEX` covers
an invalid Flat index shape. HTTP, argument and Parquet errors have no `code`.
Messages omit URL credentials, queries and fragments.

## Remote access

Only non-empty ASCII `http://` and `https://` URLs are supported. For cloud
storage, use a presigned HTTP URL or a CORS-enabled proxy. The server must
support range requests (`Accept-Ranges: bytes`). In browsers, CORS must allow
the `Range` request header and expose `Content-Range`; a server that also
exposes `ETag` must allow the `If-Match` request header.

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).
