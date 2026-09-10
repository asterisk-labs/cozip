# cozip

JavaScript reader for Cloud-Optimized ZIP archives. It reads the small index
and archive tail, verifies their integrity hash, then fetches the manifest
without scanning the ZIP Central Directory.

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

`manifest` is an array of row objects: `name`, `offset`, `size`, `cozip:location`, and the writer's extras.

`columns: [...]` picks extras. `location: false` drops `cozip:location`.

`read()` supports only Flat-profile archives (`profile = 1`). TACO archives
(`profile = 2`) and every other profile are rejected with `UNKNOWN_PROFILE`.

```js
const manifest = await read(url, {
  columns: ["cloud_pct", "split"],
  location: false,
});
```

Only non-empty ASCII `http://` and `https://` URLs are supported. For cloud
storage, use a presigned HTTP URL or a CORS-enabled proxy. The server must
support range requests (`Accept-Ranges: bytes`) and, in browsers, allow the
`Range` header through CORS.

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).
