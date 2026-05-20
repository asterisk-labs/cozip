import { test } from "node:test";
import assert from "node:assert/strict";
import { read } from "../src/index.js";

const URL = "https://huggingface.co/datasets/Major-TOM/Core-VIIRS-Nighttime-Light/resolve/main/2024/MAJORTOM-VIIRS-NTL_2024_median_000.zip";

test("returns rows with name, offset, size, cozip:gdal_vsi", async () => {
  const manifest = await read(URL);
  assert.ok(Array.isArray(manifest));
  assert.ok(manifest.length > 0);

  const row = manifest[0];
  assert.ok("name" in row);
  assert.ok("offset" in row);
  assert.ok("size" in row);
  assert.ok("cozip:gdal_vsi" in row);
});

test("cozip:gdal_vsi uses /vsisubfile + /vsicurl", async () => {
  const manifest = await read(URL);
  const row = manifest[0];
  assert.equal(
    row["cozip:gdal_vsi"],
    `/vsisubfile/${row.offset}_${row.size},/vsicurl/${URL}`,
  );
});

test("gdalVsi: false drops the VSI column", async () => {
  const manifest = await read(URL, { gdalVsi: false });
  assert.ok(manifest.length > 0);
  assert.ok(!("cozip:gdal_vsi" in manifest[0]));
});

test("columns: [...] keeps requested extras alongside name/offset/size", async () => {
  const all = await read(URL);
  const extraKey = Object.keys(all[0]).find(
    (k) => !["name", "offset", "size", "cozip:gdal_vsi"].includes(k),
  );
  if (!extraKey) return; // nothing to test if the archive has no extras

  const filtered = await read(URL, { columns: [extraKey] });
  const keys = Object.keys(filtered[0]).sort();
  assert.deepEqual(
    keys,
    ["cozip:gdal_vsi", extraKey, "name", "offset", "size"].sort(),
  );
});

test("rejects non-http URLs", async () => {
  await assert.rejects(() => read("s3://bucket/key.zip"), /only http\(s\)/);
  await assert.rejects(() => read("gs://bucket/key.zip"), /only http\(s\)/);
  await assert.rejects(() => read("file:///tmp/x.zip"), /only http\(s\)/);
});