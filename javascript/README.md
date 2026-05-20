# cozip

JavaScript reader for Cloud-Optimized ZIP archives. One HTTP call gives you the manifest of a remote `.zip`.

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

`manifest` is an array of row objects with `name`, `offset`, `size`, `cozip:gdal_vsi`, plus whatever extras the writer added. Pass `columns: [...]` to bring only specific extras, `gdalVsi: false` to drop the VSI path column.

```js
const manifest = await read(url, {
  columns: ["cloud_pct", "split"],
  gdalVsi: false,
});
```

Only `http://` and `https://` URLs are supported. Cloud schemes like `s3://` or `gcs://` are out of scope, use a presigned HTTP URL or a CORS-enabled proxy. The server must support range requests (`Accept-Ranges: bytes`) and, for browser use, CORS with `Range` allowed.

## Spec

See [SPEC.md](https://github.com/asterisk-labs/cozip/blob/main/SPEC.md) for the on-disk format.

## License

MIT. See [LICENSE](../LICENSE).