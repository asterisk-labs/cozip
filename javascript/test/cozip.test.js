import { test } from "node:test";
import assert from "node:assert/strict";
import { read } from "../src/index.js";

const URL =
  "https://raw.githubusercontent.com/asterisk-labs/cozip_reader/main/test/data/flat_simple.zip";

function structuralArchive(entryName = "__metadata__") {
  const bytes = new Uint8Array(32768 + 51);
  const view = new DataView(bytes.buffer);
  const encoder = new TextEncoder();
  const name = encoder.encode(entryName);
  const payloadSize = 11 + 2 + name.length + 8 + 8;
  view.setUint32(0, 0x04034b50, true);
  view.setUint32(18, payloadSize, true);
  view.setUint32(22, payloadSize, true);
  view.setUint16(26, 9, true);
  view.setUint16(28, 12, true);
  bytes.set(encoder.encode("__cozip__"), 30);
  view.setUint16(39, 0xca0c, true);
  view.setUint16(41, 8, true);
  bytes.set(encoder.encode("CZIP"), 51);
  view.setUint16(55, 1, true);
  view.setUint8(57, 1);
  view.setUint32(58, 1, true);
  view.setUint16(62, name.length, true);
  bytes.set(name, 64);
  const offsetPos = 64 + name.length;
  view.setBigUint64(offsetPos, 100n, true);
  view.setBigUint64(offsetPos + 8, 1n, true);

  let hash = 0xcbf29ce484222325n;
  for (const byte of bytes.subarray(51)) {
    hash ^= BigInt(byte);
    hash = (hash * 0x100000001b3n) & 0xffffffffffffffffn;
  }
  view.setBigUint64(43, hash, true);
  return bytes;
}

test("returns rows with name, offset, size, cozip:location", async () => {
  const manifest = await read(URL);
  assert.ok(Array.isArray(manifest));
  assert.ok(manifest.length > 0);

  const row = manifest[0];
  assert.ok("name" in row);
  assert.ok("offset" in row);
  assert.ok("size" in row);
  assert.ok("cozip:location" in row);
});

test("cozip:location uses /vsisubfile + /vsicurl", async () => {
  const manifest = await read(URL);
  const row = manifest[0];
  assert.equal(
    row["cozip:location"],
    `/vsisubfile/${row.offset}_${row.size},/vsicurl/${URL}`,
  );
});

test("location: false drops the VSI column", async () => {
  const manifest = await read(URL, { location: false });
  assert.ok(manifest.length > 0);
  assert.ok(!("cozip:location" in manifest[0]));
});

test("does not ask Parquet for the synthetic VSI column", async () => {
  const manifest = await read(URL, { columns: ["cozip:location"] });
  assert.ok(manifest.length > 0);
  assert.deepEqual(
    Object.keys(manifest[0]).sort(),
    ["cozip:location", "name", "offset", "size"].sort(),
  );
});

test("does not expose protected location columns from Parquet", async () => {
  const manifest = await read(URL, {
    columns: ["cozip:location", "taco:location"],
    location: false,
  });
  assert.ok(manifest.length > 0);
  assert.deepEqual(Object.keys(manifest[0]).sort(), ["name", "offset", "size"].sort());
});

test("columns: [...] keeps requested extras alongside name/offset/size", async () => {
  const all = await read(URL);
  const extraKey = Object.keys(all[0]).find(
    (k) => !["name", "offset", "size", "cozip:location"].includes(k),
  );
  if (!extraKey) return; // nothing to test if the archive has no extras

  const filtered = await read(URL, { columns: [extraKey] });
  const keys = Object.keys(filtered[0]).sort();
  assert.deepEqual(
    keys,
    ["cozip:location", extraKey, "name", "offset", "size"].sort(),
  );
});

test("rejects non-http URLs", async () => {
  await assert.rejects(() => read("s3://bucket/key.zip"), /only http\(s\)/);
  await assert.rejects(() => read("gs://bucket/key.zip"), /only http\(s\)/);
  await assert.rejects(() => read("file:///tmp/x.zip"), /only http\(s\)/);
});

test("rejects non-ASCII URLs before fetching", async () => {
  await assert.rejects(
    () => read("https://example.test/niño.zip"),
    /contains non-ASCII characters/,
  );
});

test("rejects an empty URL before fetching", async () => {
  await assert.rejects(() => read(""), /must not be empty/);
});

test("rejects a malformed index count without over-reading", async () => {
  const bytes = new Uint8Array(32768 + 51);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0x04034b50, true);
  view.setUint32(18, 11, true);
  view.setUint32(22, 11, true);
  view.setUint16(26, 9, true);
  view.setUint16(28, 12, true);
  bytes.set(new TextEncoder().encode("__cozip__"), 30);
  view.setUint16(39, 0xca0c, true);
  view.setUint16(41, 8, true);
  bytes.set(new TextEncoder().encode("CZIP"), 51);
  view.setUint16(55, 1, true);
  view.setUint8(57, 1);
  view.setUint32(58, 1, true);

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(bytes, { status: 200 });
  try {
    await assert.rejects(
      () => read("https://example.test/malformed.zip"),
      /entry count does not fit/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("rejects every tested non-Flat profile, including TACO profile 2", async () => {
  const originalFetch = globalThis.fetch;
  try {
    for (const profile of [0, 2, 255]) {
      const bytes = structuralArchive();
      new DataView(bytes.buffer).setUint8(57, profile);
      globalThis.fetch = async () => new Response(bytes, { status: 200 });
      await assert.rejects(
        () => read(`https://example.test/profile-${profile}.zip`),
        new RegExp(`UNKNOWN_PROFILE.*Flat profile 1.*profile ${profile}`),
      );
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("verifies the integrity hash before fetching metadata", async () => {
  const bytes = structuralArchive();
  bytes[100] ^= 1;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(bytes, { status: 200 });
  try {
    await assert.rejects(
      () => read("https://example.test/corrupt.zip"),
      /HASH_MISMATCH/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("rejects the reserved padding name in the index", async () => {
  const bytes = structuralArchive("__cozip_padding__");

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(bytes, { status: 200 });
  try {
    await assert.rejects(
      () => read("https://example.test/padding-index.zip"),
      /must not list the reserved __cozip_padding__ entry/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("accepts a 206 response clamped to the end of a small archive", async () => {
  const bytes = structuralArchive();
  bytes[100] ^= 1;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () =>
    new Response(bytes, {
      status: 206,
      headers: { "Content-Range": `bytes 0-${bytes.length - 1}/${bytes.length}` },
    });
  try {
    await assert.rejects(
      () => read("https://example.test/small.zip"),
      /HASH_MISMATCH/,
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("validates options before fetching", async () => {
  await assert.rejects(
    () => read("https://example.test/x.zip", { columns: "name" }),
    /columns must be an array/,
  );
  await assert.rejects(
    () => read("https://example.test/x.zip", { columns: [""] }),
    /non-empty strings/,
  );
  await assert.rejects(
    () => read("https://example.test/x.zip", { location: "yes" }),
    /location must be a boolean/,
  );
});
