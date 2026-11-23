import { test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { crc32 } from "node:zlib";
import { read, verify } from "../src/index.js";

// cozip-api-fixtures v0.1.0: a 32,819-byte Flat archive with 12 entries.
const FIXTURE_URL =
  "https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip";

// A two-row Parquet manifest with name, offset, size and split, plus stored
// copies of the reader-owned columns cozip:location, Taco:Location and
// COZIP:GDAL_VSI that a reader must never return. Written by pyarrow with
// compression="none", use_dictionary=False, write_statistics=False and
// store_schema=False.
const MANIFEST = new Uint8Array(
  Buffer.from(
    `
UEFSMRUAFTAVMCwVBBUAFQYVBhwAAAACAAAABAEFAAAAYS50aWYFAAAAYi50aWYVABUsFSwsFQQV
ABUGFQYcAAAAAgAAAAQByAAAAAAAAAAsAQAAAAAAABUAFSwVLCwVBBUAFQYVBhwAAAACAAAABAEK
AAAAAAAAABQAAAAAAAAAFQAVLhUuLBUEFQAVBhUGHAAAAAIAAAAEAQUAAAB0cmFpbgQAAAB0ZXN0
FQAVNBU0LBUEFQAVBhUGHAAAAAIAAAAEAQYAAABzdG9yZWQGAAAAc3RvcmVkFQAVNBU0LBUEFQAV
BhUGHAAAAAIAAAAEAQYAAABzdG9yZWQGAAAAc3RvcmVkFQAVNBU0LBUEFQAVBhUGHAAAAAIAAAAE
AQYAAABzdG9yZWQGAAAAc3RvcmVkFQQZjDUAGAZzY2hlbWEVDgAVDCUCGARuYW1lJQBMHAAAABUE
JQIYBm9mZnNldCUcTKwTQBIAAAAVBCUCGARzaXplJRxMrBNAEgAAABUMJQIYBXNwbGl0JQBMHAAA
ABUMJQIYDmNvemlwOmxvY2F0aW9uJQBMHAAAABUMJQIYDVRhY286TG9jYXRpb24lAEwcAAAAFQwl
AhgOQ09aSVA6R0RBTF9WU0klAEwcAAAAFgQZHBl8JgAcFQwZJQYAGRgEbmFtZRUAFgQWVhZWJghJ
HBUAFQAVAgA8FhQZBhkmAAQAAAAmABwVBBklBgAZGAZvZmZzZXQVABYEFlIWUiZeSRwVABUAFQIA
PCkGGSYABAAAACYAHBUEGSUGABkYBHNpemUVABYEFlIWUiawAUkcFQAVABUCADwpBhkmAAQAAAAm
ABwVDBklBgAZGAVzcGxpdBUAFgQWVBZUJoICSRwVABUAFQIAPBYSGQYZJgAEAAAAJgAcFQwZJQYA
GRgOY296aXA6bG9jYXRpb24VABYEFloWWibWAkkcFQAVABUCADwWGBkGGSYABAAAACYAHBUMGSUG
ABkYDVRhY286TG9jYXRpb24VABYEFloWWiawA0kcFQAVABUCADwWGBkGGSYABAAAACYAHBUMGSUG
ABkYDkNPWklQOkdEQUxfVlNJFQAWBBZaFlomigRJHBUAFQAVAgA8FhgZBhkmAAQAAAAW3AQWBCYI
FtwEACggcGFycXVldC1jcHAtYXJyb3cgdmVyc2lvbiAyNS4wLjEZfBwAABwAABwAABwAABwAABwA
ABwAAABjAgAAUEFSMQ==`,
    "base64",
  ),
);

const HASH_WINDOW = 32768;
const encoder = new TextEncoder();

/**
 * Writes FNV-1a 64 over the index payload and the last 32 KiB into bytes
 * 43-50, as a writer finalizes an archive.
 */
function seal(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const indexEnd = 51 + view.getUint32(18, true);
  const suffixStart = bytes.length - HASH_WINDOW;
  let hash = 0xcbf29ce484222325n;
  const add = (part) => {
    for (const byte of part) {
      hash ^= BigInt(byte);
      hash = (hash * 0x100000001b3n) & 0xffffffffffffffffn;
    }
  };
  add(bytes.subarray(51, indexEnd));
  add(bytes.subarray(Math.max(suffixStart, indexEnd)));
  view.setBigUint64(43, hash, true);
  return bytes;
}

/**
 * Builds a sealed Flat cozip archive: __cozip__, an optional entry of
 * `before` bytes, __metadata__, an optional __cozip_padding__ of `after`
 * bytes, the Central Directory and the EOCD. `extra` is the manifest's local
 * extra field; `zip64` records its sizes as 0xFFFFFFFF with a ZIP64 extra.
 * Returns the bytes, the manifest offset and its local header position.
 */
function flatArchive({
  before = 0,
  after = HASH_WINDOW,
  manifest = MANIFEST,
  extra = new Uint8Array(0),
  zip64 = false,
} = {}) {
  const cozipExtra = new Uint8Array([0x0c, 0xca, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  const entries = [{ name: "__cozip__", data: new Uint8Array(41), extra: cozipExtra }];
  if (before > 0) entries.push({ name: "before.bin", data: new Uint8Array(before) });
  const meta = { name: "__metadata__", data: manifest, extra, zip64 };
  entries.push(meta);
  if (after > 0) entries.push({ name: "__cozip_padding__", data: new Uint8Array(after) });

  // The Flat index, once the manifest offset is known.
  const bytes = zipArchive(entries, ([cozip]) => {
    const index = new DataView(cozip.data.buffer);
    cozip.data.set(encoder.encode("CZIP"), 0);
    index.setUint16(4, 1, true);
    index.setUint8(6, 1);
    index.setUint32(7, 1, true);
    index.setUint16(11, 12, true);
    cozip.data.set(encoder.encode("__metadata__"), 13);
    index.setBigUint64(25, BigInt(meta.offset), true);
    index.setBigUint64(33, BigInt(manifest.length), true);
  });
  return { bytes, offset: meta.offset, header: meta.header };
}

/**
 * Builds a sealed cozip archive of any profile: __cozip__, the files in
 * order, an optional __cozip_padding__ of `after` bytes, the Central
 * Directory and the EOCD. Files marked `indexed` are the priority files,
 * listed in the index in their order.
 */
function indexedArchive({ profile = 0, files, after = HASH_WINDOW }) {
  const cozipExtra = new Uint8Array([0x0c, 0xca, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  const names = files.filter((file) => file.indexed).map((file) => encoder.encode(file.name));
  const size = 11 + names.reduce((total, name) => total + 18 + name.length, 0);
  const entries = [
    { name: "__cozip__", data: new Uint8Array(size), extra: cozipExtra },
    ...files.map(({ name, data, indexed }) => ({ name, data, indexed })),
  ];
  if (after > 0) entries.push({ name: "__cozip_padding__", data: new Uint8Array(after) });

  return zipArchive(entries, ([cozip, ...rest]) => {
    const index = new DataView(cozip.data.buffer);
    const listed = rest.filter((entry) => entry.indexed);
    cozip.data.set(encoder.encode("CZIP"), 0);
    index.setUint16(4, 1, true);
    index.setUint8(6, profile);
    index.setUint32(7, listed.length, true);
    let at = 11;
    for (const name of names) {
      index.setUint16(at, name.length, true);
      at += 2;
    }
    for (const name of names) {
      cozip.data.set(name, at);
      at += name.length;
    }
    for (const entry of listed) {
      index.setBigUint64(at, BigInt(entry.offset), true);
      at += 8;
    }
    for (const entry of listed) {
      index.setBigUint64(at, BigInt(entry.data.length), true);
      at += 8;
    }
  });
}

/**
 * Lays out `entries` as STORE ZIP entries, lets `fillIndex` write the index
 * payload into entries[0].data once every offset is known, and returns the
 * sealed archive: local headers, Central Directory and EOCD.
 */
function zipArchive(entries, fillIndex) {
  let at = 0;
  for (const entry of entries) {
    entry.nameBytes = encoder.encode(entry.name);
    entry.localExtra = entry.zip64
      ? concatBytes(zip64Extra(entry.data.length), entry.extra)
      : (entry.extra ?? new Uint8Array(0));
    entry.header = at;
    entry.offset = at + 30 + entry.nameBytes.length + entry.localExtra.length;
    at = entry.offset + entry.data.length;
  }
  fillIndex(entries);

  const parts = [];
  for (const entry of entries) {
    const header = new DataView(new ArrayBuffer(30));
    const size = entry.zip64 ? 0xffffffff : entry.data.length;
    header.setUint32(0, 0x04034b50, true);
    header.setUint16(4, entry.zip64 ? 45 : 20, true);
    header.setUint16(12, 0x21, true);
    header.setUint32(14, crc32(entry.data), true);
    header.setUint32(18, size, true);
    header.setUint32(22, size, true);
    header.setUint16(26, entry.nameBytes.length, true);
    header.setUint16(28, entry.localExtra.length, true);
    parts.push(new Uint8Array(header.buffer), entry.nameBytes, entry.localExtra, entry.data);
  }
  const cdStart = at;
  for (const entry of entries) {
    const header = new DataView(new ArrayBuffer(46));
    const size = entry.zip64 ? 0xffffffff : entry.data.length;
    const cdExtra = entry.zip64 ? zip64Extra(entry.data.length) : new Uint8Array(0);
    header.setUint32(0, 0x02014b50, true);
    header.setUint16(4, entry.zip64 ? 45 : 20, true);
    header.setUint16(6, entry.zip64 ? 45 : 20, true);
    header.setUint16(14, 0x21, true);
    header.setUint32(16, crc32(entry.data), true);
    header.setUint32(20, size, true);
    header.setUint32(24, size, true);
    header.setUint16(28, entry.nameBytes.length, true);
    header.setUint16(30, cdExtra.length, true);
    header.setUint32(42, entry.header, true);
    parts.push(new Uint8Array(header.buffer), entry.nameBytes, cdExtra);
    at += 46 + entry.nameBytes.length + cdExtra.length;
  }
  const eocd = new DataView(new ArrayBuffer(22));
  eocd.setUint32(0, 0x06054b50, true);
  eocd.setUint16(8, entries.length, true);
  eocd.setUint16(10, entries.length, true);
  eocd.setUint32(12, at - cdStart, true);
  eocd.setUint32(16, cdStart, true);
  parts.push(new Uint8Array(eocd.buffer));

  return seal(concatBytes(...parts));
}

function zip64Extra(size) {
  const extra = new DataView(new ArrayBuffer(20));
  extra.setUint16(0, 0x0001, true);
  extra.setUint16(2, 16, true);
  extra.setBigUint64(4, BigInt(size), true);
  extra.setBigUint64(12, BigInt(size), true);
  return new Uint8Array(extra.buffer);
}

function concatBytes(...parts) {
  const out = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function viewOf(bytes) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

/**
 * Serves `bytes` with Range support on 127.0.0.1 and records every request.
 * `etag` is the ETag header (null for none). `ignoreRange` answers 200 with
 * the whole object. `next` ({ bytes, etag, from }) serves another version
 * from request number `from` on. `ifMatch` answers 412 when If-Match names
 * another version. `status` answers every request with that status.
 */
async function withServer(bytes, options, run) {
  const { etag = '"v1"', ignoreRange = false, next = null, ifMatch = true, status = 0 } =
    options;
  const requests = [];
  const server = createServer((req, res) => {
    const version = next && requests.length >= next.from ? next : { bytes, etag };
    requests.push({ url: req.url, range: req.headers.range, ifMatch: req.headers["if-match"] });
    const headers = version.etag ? { ETag: version.etag } : {};
    const wanted = req.headers["if-match"];
    if (status || (ifMatch && wanted && wanted !== version.etag)) {
      res.writeHead(status || 412, headers);
      res.end();
      return;
    }
    const body = version.bytes;
    const match = /^bytes=(\d+)-(\d+)$/.exec(req.headers.range ?? "");
    if (ignoreRange || !match) {
      res.writeHead(200, { ...headers, "Content-Length": body.length });
      res.end(body);
      return;
    }
    const start = Number(match[1]);
    const end = Math.min(Number(match[2]), body.length - 1);
    res.writeHead(206, {
      ...headers,
      "Content-Range": `bytes ${start}-${end}/${body.length}`,
      "Content-Length": end - start + 1,
    });
    res.end(body.subarray(start, end + 1));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  try {
    return await run(`http://127.0.0.1:${port}/archive.zip`, requests);
  } finally {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  }
}

async function withMockFetch(bytes, run) {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(bytes, { status: 200 });
  try {
    return await run();
  } finally {
    globalThis.fetch = originalFetch;
  }
}

const EXPECTED_ROWS = [
  { name: "a.tif", offset: 200n, size: 10n, split: "train" },
  { name: "b.tif", offset: 300n, size: 20n, split: "test" },
];

function withoutLocation(rows) {
  return rows.map(({ "cozip:location": _, ...row }) => row);
}

// Layouts of a Flat archive, from the archive of the pinned fixture to a
// manifest away from both ends.
function middleArchive(options = {}) {
  const archive = flatArchive({ before: 100000, after: 100000, ...options });
  const { bytes, offset } = archive;
  assert.ok(offset > 65536 && offset + MANIFEST.length < bytes.length - HASH_WINDOW);
  return archive;
}

// Remote reads of the pinned fixture.

test("returns rows with name, offset, size, cozip:location", async () => {
  const manifest = await read(FIXTURE_URL);
  assert.ok(Array.isArray(manifest));
  assert.equal(manifest.length, 12);

  const row = manifest[0];
  assert.ok("name" in row);
  assert.ok("offset" in row);
  assert.ok("size" in row);
  assert.ok("cozip:location" in row);
});

test("cozip:location uses /vsisubfile + /vsicurl", async () => {
  const manifest = await read(FIXTURE_URL);
  const row = manifest[0];
  assert.equal(
    row["cozip:location"],
    `/vsisubfile/${row.offset}_${row.size},/vsicurl/${FIXTURE_URL}`,
  );
});

test("location: false drops the VSI column", async () => {
  const manifest = await read(FIXTURE_URL, { location: false });
  assert.ok(manifest.length > 0);
  assert.ok(!("cozip:location" in manifest[0]));
});

test("does not ask Parquet for the synthetic VSI column", async () => {
  const manifest = await read(FIXTURE_URL, { columns: ["cozip:location"] });
  assert.ok(manifest.length > 0);
  assert.deepEqual(
    Object.keys(manifest[0]).sort(),
    ["cozip:location", "name", "offset", "size"].sort(),
  );
});

test("does not expose protected location columns from Parquet", async () => {
  const manifest = await read(FIXTURE_URL, {
    columns: ["cozip:location", "taco:location"],
    location: false,
  });
  assert.ok(manifest.length > 0);
  assert.deepEqual(Object.keys(manifest[0]).sort(), ["name", "offset", "size"].sort());
});

test("columns: [...] keeps requested extras alongside name/offset/size", async () => {
  const filtered = await read(FIXTURE_URL, { columns: ["continent"] });
  const keys = Object.keys(filtered[0]).sort();
  assert.deepEqual(keys, ["continent", "cozip:location", "name", "offset", "size"].sort());
});

test("reads the pinned fixture with exactly one request", async () => {
  const originalFetch = globalThis.fetch;
  const ranges = [];
  globalThis.fetch = async (input, init) => {
    ranges.push(init?.headers?.Range);
    return originalFetch(input, init);
  };
  try {
    const manifest = await read(FIXTURE_URL);
    assert.equal(manifest.length, 12);
  } finally {
    globalThis.fetch = originalFetch;
  }
  assert.deepEqual(ranges, ["bytes=0-65535"]);
});

// Arguments.

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

// Reusing bytes already in hand.

test("reads a small archive with one request", async () => {
  const { bytes } = flatArchive();
  assert.ok(bytes.length < 65536);
  await withServer(bytes, {}, async (url, requests) => {
    const rows = await read(url);
    assert.deepEqual(withoutLocation(rows), EXPECTED_ROWS);
    assert.equal(rows[0]["cozip:location"], `/vsisubfile/200_10,/vsicurl/${url}`);
    assert.deepEqual(requests.map((r) => r.range), ["bytes=0-65535"]);
  });
});

test("reads a manifest inside the head of a large archive from memory", async () => {
  const { bytes, offset } = flatArchive({ after: 200000 });
  assert.ok(offset + MANIFEST.length < 65536);
  await withServer(bytes, {}, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.deepEqual(
      requests.map((r) => r.range),
      ["bytes=0-65535", `bytes=${bytes.length - HASH_WINDOW}-${bytes.length - 1}`],
    );
  });
});

test("reads a manifest inside the hash suffix from memory", async () => {
  const { bytes, offset } = flatArchive({ before: 200000, after: 1000 });
  assert.ok(offset > bytes.length - HASH_WINDOW);
  await withServer(bytes, {}, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.equal(requests.length, 2);
  });
});

test("reads a manifest across the head and a contiguous suffix from memory", async () => {
  const { bytes, offset } = flatArchive({ before: 64962, after: 10000 });
  assert.ok(offset < 65536 && offset + MANIFEST.length > 65536);
  assert.ok(bytes.length - HASH_WINDOW < 65536 && bytes.length > 65536);
  await withServer(bytes, {}, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.deepEqual(
      requests.map((r) => r.range),
      ["bytes=0-65535", `bytes=65536-${bytes.length - 1}`],
    );
  });
});

test("fetches a manifest away from both ends with its local header", async () => {
  const { bytes, offset } = middleArchive();
  await withServer(bytes, {}, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.equal(requests.length, 3);
    assert.equal(
      requests[2].range,
      `bytes=${offset - 42 - 1024}-${offset + MANIFEST.length - 1}`,
    );
  });
});

test("fetches a manifest that straddles the start of the hash suffix", async () => {
  const { bytes, offset } = flatArchive({ before: 100000, after: 32000 });
  const suffixStart = bytes.length - HASH_WINDOW;
  assert.ok(offset < suffixStart && offset + MANIFEST.length > suffixStart);
  await withServer(bytes, {}, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.equal(requests.length, 3);
  });
});

test("never looks back past the end of the index", async () => {
  // A manifest right after the index that ends beyond the head; its bytes
  // are not Parquet, so the read fails after the request.
  const manifest = new Uint8Array(70000);
  const { bytes, offset } = flatArchive({ manifest, after: 40000 });
  assert.equal(offset, 51 + 41 + 42);
  await withServer(bytes, {}, async (url, requests) => {
    await assert.rejects(() => read(url), /parquet/);
    assert.equal(requests[2].range, `bytes=92-${offset + manifest.length - 1}`);
  });
});

// A server that ignores Range.

test("uses a 200 response with the whole object for everything", async () => {
  const { bytes } = middleArchive();
  await withServer(bytes, { ignoreRange: true }, async (url, requests) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
    assert.equal(requests.length, 1);
  });
});

test("accepts a 200 response with the whole object from fetch", async () => {
  const { bytes } = flatArchive();
  await withMockFetch(bytes, async () => {
    assert.deepEqual(
      withoutLocation(await read("https://example.test/whole.zip")),
      EXPECTED_ROWS,
    );
  });
});

test("accepts a 206 response clamped to the end of a small archive", async () => {
  const { bytes } = flatArchive();
  bytes[bytes.length - 100] ^= 1;

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () =>
    new Response(bytes, {
      status: 206,
      headers: { "Content-Range": `bytes 0-${bytes.length - 1}/${bytes.length}` },
    });
  try {
    await assert.rejects(() => read("https://example.test/small.zip"), /HASH_MISMATCH/);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

// Pinning the object version.

test("sends the strong ETag of the first response as If-Match", async () => {
  const { bytes } = middleArchive();
  await withServer(bytes, { etag: '"abc"' }, async (url, requests) => {
    await read(url);
    assert.deepEqual(
      requests.map((r) => r.ifMatch),
      [undefined, '"abc"', '"abc"'],
    );
  });
});

test("does not pin a weak or missing ETag", async () => {
  const { bytes } = middleArchive();
  for (const etag of ['W/"abc"', null]) {
    await withServer(bytes, { etag }, async (url, requests) => {
      assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
      assert.deepEqual(
        requests.map((r) => r.ifMatch),
        [undefined, undefined, undefined],
      );
    });
  }
});

test("fails when the server answers If-Match with 412", async () => {
  const { bytes } = middleArchive();
  const next = { bytes, etag: '"v2"', from: 1 };
  await withServer(bytes, { next }, async (url, requests) => {
    await assert.rejects(() => read(url), /archive changed while reading \(HTTP 412/);
    assert.equal(requests.length, 2);
  });
});

test("fails when a later response carries another ETag", async () => {
  const { bytes } = middleArchive();
  const next = { bytes, etag: '"v2"', from: 2 };
  await withServer(bytes, { next, ifMatch: false }, async (url) => {
    await assert.rejects(() => read(url), /archive changed while reading \(ETag "v1"/);
  });
});

test("accepts a later weak form of the pinned ETag", async () => {
  const { bytes } = middleArchive();
  const next = { bytes, etag: 'W/"v1"', from: 1 };
  await withServer(bytes, { next, ifMatch: false }, async (url) => {
    assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
  });
});

test("without an ETag, fails when the archive size changes", async () => {
  const { bytes } = middleArchive();
  const next = { bytes: concatBytes(bytes, new Uint8Array(1)), etag: null, from: 1 };
  await withServer(bytes, { etag: null, next }, async (url) => {
    await assert.rejects(() => read(url), /archive size changed while reading/);
  });
});

test("without an ETag, checks the manifest header against a replaced archive", async () => {
  // Same size, same tail, but the manifest moved 100 bytes later.
  const { bytes } = middleArchive();
  const moved = middleArchive({ before: 100100, after: 99900 }).bytes;
  assert.equal(moved.length, bytes.length);
  const next = { bytes: moved, etag: null, from: 2 };
  await withServer(bytes, { etag: null, next }, async (url) => {
    await assert.rejects(() => read(url), {
      code: "MISSING_ENTRY",
      message: /before __metadata__ does not match the index; the archive may have changed/,
    });
  });
});

test("accepts manifest headers with extra fields and ZIP64 sizes", async () => {
  const unknownExtra = new Uint8Array([0x75, 0x78, 4, 0, 1, 2, 3, 4]);
  const extra1020 = new Uint8Array(1024);
  new DataView(extra1020.buffer).setUint16(0, 0x7875, true);
  new DataView(extra1020.buffer).setUint16(2, 1020, true);
  for (const options of [
    { extra: unknownExtra },
    { extra: extra1020 },
    { zip64: true },
    { zip64: true, extra: unknownExtra },
  ]) {
    for (const { bytes } of [middleArchive(options), flatArchive(options)]) {
      await withServer(bytes, {}, async (url) => {
        assert.deepEqual(withoutLocation(await read(url)), EXPECTED_ROWS);
      });
    }
  }
});

test("rejects a manifest whose local header does not match the index", async () => {
  const cases = {
    "a wrong signature": (v, at) => v.setUint32(at, 0x04034b51, true),
    "a wrong name": (v, at) => v.setUint8(at + 41, 0x41),
    "an encryption flag": (v, at) => v.setUint16(at + 6, 1, true),
    "DEFLATE": (v, at) => v.setUint16(at + 8, 8, true),
    "a wrong compressed size": (v, at) => v.setUint32(at + 18, MANIFEST.length + 1, true),
    "a wrong uncompressed size": (v, at) => v.setUint32(at + 22, MANIFEST.length - 1, true),
    "ZIP64 sizes without a ZIP64 extra": (v, at) => {
      v.setUint32(at + 18, 0xffffffff, true);
      v.setUint32(at + 22, 0xffffffff, true);
    },
  };
  for (const [label, corrupt] of Object.entries(cases)) {
    for (const archive of [middleArchive(), flatArchive()]) {
      const bytes = archive.bytes.slice();
      corrupt(viewOf(bytes), archive.header);
      await withServer(bytes, {}, async (url) => {
        await assert.rejects(() => read(url), { code: "MISSING_ENTRY" }, label);
      });
    }
  }
});

test("rejects ZIP64 manifest sizes that do not match the index", async () => {
  const archive = middleArchive({ zip64: true });
  const bytes = archive.bytes.slice();
  viewOf(bytes).setBigUint64(archive.header + 42 + 4, BigInt(MANIFEST.length + 1), true);
  await withServer(bytes, {}, async (url) => {
    await assert.rejects(() => read(url), { code: "MISSING_ENTRY" });
  });
});

test("does not look back more than 1024 extra field bytes", async () => {
  const extra = new Uint8Array(1025);
  new DataView(extra.buffer).setUint16(0, 0x7875, true);
  new DataView(extra.buffer).setUint16(2, 1021, true);
  const { bytes } = middleArchive({ extra });
  await withServer(bytes, {}, async (url) => {
    await assert.rejects(() => read(url), { code: "MISSING_ENTRY" });
  });
});

// The fixed header and the Flat shape, checked before any further request.

test("rejects a malformed header from the first response alone", async () => {
  const cases = [
    ["INVALID_LFH", (v) => v.setUint32(0, 0x04034b51, true)],
    ["INVALID_LFH", (v) => v.setUint16(6, 0x0008, true)],
    ["INVALID_LFH", (v) => v.setUint16(6, 0x0001, true)],
    ["INVALID_LFH", (v) => v.setUint16(8, 8, true)],
    ["INVALID_LFH", (v) => v.setUint32(22, 42, true)],
    ["INVALID_LFH", (v) => (v.setUint32(18, 0, true), v.setUint32(22, 0, true))],
    ["INVALID_LFH", (v) => v.setUint16(26, 10, true)],
    ["INVALID_LFH", (v) => v.setUint16(28, 16, true)],
    ["INVALID_LFH", (v) => v.setUint8(38, 0x41)],
    ["INVALID_LFH", (v) => v.setUint16(39, 0xca0d, true)],
    ["INVALID_LFH", (v) => v.setUint16(41, 9, true)],
    ["TRUNCATED_INDEX", (v) => (v.setUint32(18, 5, true), v.setUint32(22, 5, true))],
    ["INVALID_MAGIC", (v) => v.setUint8(51, 0x58)],
    ["UNSUPPORTED_VERSION", (v) => v.setUint16(55, 0, true)],
    ["UNSUPPORTED_VERSION", (v) => v.setUint16(55, 2, true)],
    ["UNKNOWN_PROFILE", (v) => v.setUint8(57, 0)],
    ["UNKNOWN_PROFILE", (v) => v.setUint8(57, 2)],
    ["UNKNOWN_PROFILE", (v) => v.setUint8(57, 255)],
    ["INVALID_INDEX", (v) => v.setUint32(58, 2, true)],
    ["INVALID_INDEX", (v) => v.setUint32(58, 0, true)],
    ["INVALID_INDEX", (v) => (v.setUint32(18, 46, true), v.setUint32(22, 46, true))],
    ["INVALID_INDEX", (v) => v.setUint16(62, 11, true)],
  ];
  const { bytes: base } = middleArchive();
  for (const [code, corrupt] of cases) {
    const bytes = base.slice();
    corrupt(viewOf(bytes));
    await withServer(bytes, {}, async (url, requests) => {
      await assert.rejects(() => read(url), { code, message: new RegExp(`^cozip ${code}: `) });
      assert.equal(requests.length, 1, `${code} after one request`);
    });
  }
});

test("names the profile when it is not Flat", async () => {
  for (const profile of [0, 2, 255]) {
    const { bytes } = flatArchive();
    viewOf(bytes).setUint8(57, profile);
    await withMockFetch(bytes, async () => {
      await assert.rejects(() => read(`https://example.test/profile-${profile}.zip`), {
        code: "UNKNOWN_PROFILE",
        message: new RegExp(`UNKNOWN_PROFILE.*Flat profile 1.*profile ${profile}`),
      });
    });
  }
});

test("rejects an archive below the minimum size", async () => {
  const { bytes } = flatArchive({ after: 0 });
  assert.ok(bytes.length < 32768 + 51);
  for (const small of [bytes, bytes.subarray(0, 40)]) {
    await withMockFetch(small, async () => {
      await assert.rejects(() => read("https://example.test/small.zip"), {
        code: "ARCHIVE_TOO_SMALL",
      });
    });
  }
});

// Checks after the hash.

test("verifies the integrity hash before fetching metadata", async () => {
  const { bytes } = middleArchive();
  bytes[bytes.length - 100] ^= 1;
  await withServer(bytes, {}, async (url, requests) => {
    await assert.rejects(() => read(url), { code: "HASH_MISMATCH" });
    assert.equal(requests.length, 2);
  });
});

test("requires a comment-free end of central directory record", async () => {
  const { bytes } = flatArchive();
  const commented = concatBytes(bytes, encoder.encode("note"));
  viewOf(commented).setUint16(commented.length - 4 - 2, 4, true);
  const claimed = bytes.slice();
  viewOf(claimed).setUint16(claimed.length - 2, 4, true);
  const missing = bytes.slice();
  viewOf(missing).setUint32(missing.length - 22, 0, true);
  for (const archive of [commented, claimed, missing]) {
    await withMockFetch(seal(archive), async () => {
      await assert.rejects(() => read("https://example.test/eocd.zip"), {
        code: "INVALID_ZIP_STRUCTURE",
      });
    });
  }
});

test("rejects a Flat index that does not name __metadata__", async () => {
  const { bytes } = flatArchive();
  bytes.set(encoder.encode("__metadata_x"), 64);
  await withMockFetch(seal(bytes), async () => {
    await assert.rejects(() => read("https://example.test/name.zip"), {
      code: "INVALID_INDEX",
      message: /no entry named __metadata__/,
    });
  });
});

test("rejects manifest ranges outside the archive", async () => {
  const cases = [
    [133n, BigInt(MANIFEST.length)],
    [51n, BigInt(MANIFEST.length)],
    [null, 0n],
    [null, 1n << 40n],
    [1n << 63n, BigInt(MANIFEST.length)],
  ];
  for (const [offset, size] of cases) {
    const { bytes } = flatArchive();
    if (offset !== null) viewOf(bytes).setBigUint64(76, offset, true);
    viewOf(bytes).setBigUint64(84, size, true);
    await withMockFetch(seal(bytes), async () => {
      await assert.rejects(() => read("https://example.test/range.zip"), {
        code: "INVALID_OFFSET",
      });
    });
  }
});

// Credentials stay out of messages.

test("keeps the query, fragment and userinfo out of HTTP errors", async () => {
  const { bytes } = flatArchive();
  await withServer(bytes, { status: 403 }, async (url) => {
    const signed = `${url}?X-Amz-Signature=topsecret&X-Amz-Credential=AKIDEXAMPLE#frag`;
    await assert.rejects(
      () => read(signed),
      (error) => {
        assert.match(error.message, /HTTP 403/);
        assert.match(error.message, /archive\.zip\?<redacted>#<redacted>/);
        assert.doesNotMatch(error.message, /topsecret|AKIDEXAMPLE|frag/);
        return true;
      },
    );
  });
});

test("keeps credentials out of fetch failures", async () => {
  const server = createServer();
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  await new Promise((resolve) => server.close(resolve));
  const urls = [
    `http://user:hunter2@127.0.0.1:${port}/a.zip`,
    `http://127.0.0.1:${port}/a.zip?X-Amz-Signature=hunter2`,
    "http://exa mple.test/a.zip?X-Amz-Signature=hunter2",
    "http://user:pa/hunter2@example.test/a.zip",
  ];
  for (const url of urls) {
    await assert.rejects(
      () => read(url),
      (error) => {
        assert.match(error.message, /^cozip: could not fetch /);
        assert.doesNotMatch(error.message, /hunter2/);
        return true;
      },
      url,
    );
  }
});

test("keeps credentials out of the scheme error", async () => {
  await assert.rejects(
    () => read("s3://AKIDEXAMPLE:hunter2@bucket/key.zip?X-Amz-Signature=hunter2"),
    (error) => {
      assert.match(error.message, /only http\(s\)/);
      assert.doesNotMatch(error.message, /hunter2|AKIDEXAMPLE/);
      return true;
    },
  );
});

// Column selection.

test("drops stored reader-owned columns in any case", async () => {
  const { bytes } = flatArchive();
  await withServer(bytes, {}, async (url) => {
    const rows = await read(url);
    assert.deepEqual(Object.keys(rows[0]), ["name", "offset", "size", "split", "cozip:location"]);
    assert.equal(rows[0]["cozip:location"], `/vsisubfile/200_10,/vsicurl/${url}`);
    const bare = await read(url, { location: false });
    assert.deepEqual(Object.keys(bare[0]), ["name", "offset", "size", "split"]);
  });
});

test("ignores reader-owned names in columns in any case", async () => {
  const { bytes } = flatArchive();
  await withServer(bytes, {}, async (url) => {
    const rows = await read(url, {
      columns: ["COZIP:LOCATION", "Taco:Location", "cozip:GDAL_VSI"],
    });
    assert.deepEqual(Object.keys(rows[0]).sort(), ["cozip:location", "name", "offset", "size"]);
    assert.equal(rows[0]["cozip:location"], `/vsisubfile/200_10,/vsicurl/${url}`);
  });
});

test("names unknown columns, matched with case", async () => {
  const { bytes } = flatArchive();
  await withServer(bytes, {}, async (url) => {
    await assert.rejects(
      () => read(url, { columns: ["split", "Split", "nope", "nope"] }),
      /columns not in the manifest: "Split", "nope"$/,
    );
    const rows = await read(url, { columns: ["split"], location: false });
    assert.deepEqual(rows, EXPECTED_ROWS);
  });
});

// verify: the index and integrity hash of an archive of any profile.

const NONE_FILES = [
  { name: "a.bin", data: encoder.encode("first"), indexed: true },
  { name: "dir/b.bin", data: encoder.encode("second payload"), indexed: true },
  { name: "c.bin", data: encoder.encode("third"), indexed: false },
  { name: "d.bin", data: encoder.encode("fourth!"), indexed: true },
];

/** Where entry i of the index lives: its name, offset and size fields. */
function indexEntry(bytes, i) {
  const v = viewOf(bytes);
  const count = v.getUint32(58, true);
  const lengths = 62;
  let name = lengths + 2 * count;
  let total = 0;
  for (let k = 0; k < count; k++) {
    const length = v.getUint16(lengths + 2 * k, true);
    if (k < i) name += length;
    total += length;
  }
  const offsets = lengths + 2 * count + total;
  return { name, offset: offsets + 8 * i, size: offsets + 8 * count + 8 * i };
}

/** A patched copy of `bytes`, sealed again when `reseal` is true. */
function patched(bytes, patch, reseal) {
  const copy = new Uint8Array(bytes);
  patch(copy, viewOf(copy));
  return reseal ? seal(copy) : copy;
}

test("verify accepts intact archives of every profile", async () => {
  const taco = [
    { name: "DATA/a.bin", data: encoder.encode("tile"), indexed: false },
    { name: "COLLECTION.json", data: encoder.encode("{}"), indexed: true },
    { name: "METADATA/level0.parquet", data: encoder.encode("PAR1"), indexed: true },
  ];
  const archives = [
    flatArchive().bytes,
    indexedArchive({ files: NONE_FILES }),
    indexedArchive({ profile: 2, files: taco }),
    // Spec 7.1: a reader may parse an index whose profile it does not know.
    indexedArchive({ profile: 7, files: NONE_FILES }),
  ];
  for (const bytes of archives) {
    await withServer(bytes, {}, async (url, requests) => {
      assert.equal(await verify(url), undefined);
      // Each is under 64 KiB, so the first response holds all of it.
      assert.equal(requests.length, 1);
    });
  }
});

test("verify reads an index that runs past the first response", async () => {
  const files = Array.from({ length: 4000 }, (_, i) => ({
    name: `e${String(i).padStart(5, "0")}.bin`,
    data: new Uint8Array([7]),
    indexed: true,
  }));
  const bytes = indexedArchive({ files });
  const indexEnd = 51 + viewOf(bytes).getUint32(18, true);
  assert.ok(indexEnd > 65536);
  await withServer(bytes, {}, async (url, requests) => {
    assert.equal(await verify(url), undefined);
    assert.deepEqual(
      requests.map(({ range }) => range),
      [
        "bytes=0-65535",
        `bytes=65536-${indexEnd - 1}`,
        `bytes=${bytes.length - HASH_WINDOW}-${bytes.length - 1}`,
      ],
    );
    assert.deepEqual(
      requests.map(({ ifMatch }) => ifMatch),
      [undefined, '"v1"', '"v1"'],
    );
  });
  // The last size lies in the second range, which is hashed and checked.
  const zero = (copy, v) => v.setBigUint64(indexEntry(copy, 3999).size, 0n, true);
  await withServer(patched(bytes, zero, false), {}, async (url) => {
    await assert.rejects(verify(url), { code: "HASH_MISMATCH" });
  });
  await withServer(patched(bytes, zero, true), {}, async (url) => {
    await assert.rejects(verify(url), {
      code: "INVALID_OFFSET",
      message: /entry 3999 \('e03999\.bin'\) has a range outside the archive/,
    });
  });
});

test("verify names the rule a broken archive breaks", async () => {
  const bytes = indexedArchive({ files: NONE_FILES });
  const name = (i, text) => (copy) => copy.set(encoder.encode(text), indexEntry(copy, i).name);
  const cases = [
    [(c) => (c[0] = 0x58), false, "INVALID_LFH", /byte 0 is not a ZIP local file header/],
    [(c) => (c[43] ^= 1), false, "HASH_MISMATCH", /integrity hash/],
    [name(0, "/"), false, "HASH_MISMATCH", /integrity hash/],
    [(c) => (c[c.length - 2] = 1), true, "INVALID_ZIP_STRUCTURE", /comment-free/],
    [(c, v) => v.setUint32(58, 1000, true), true, "TRUNCATED_INDEX", /cannot hold 1000 entries/],
    [
      (c, v) => {
        v.setUint32(18, v.getUint32(18, true) + 1, true);
        v.setUint32(22, v.getUint32(22, true) + 1, true);
      },
      true,
      "INVALID_INDEX",
      /holds only/,
    ],
    [name(0, "/"), true, "INVALID_NAME", /entry 0 name must be relative/],
    [(c) => (c[indexEntry(c, 0).name + 1] = 0x80), true, "INVALID_NAME", /non-ASCII/],
    [name(1, "dir/../bn"), true, "INVALID_NAME", /entry 1 name contains a '\.' or '\.\.'/],
    [name(1, "__cozip__"), true, "INVALID_NAME", /reserved name '__cozip__'/],
    [
      (c, v) => v.setBigUint64(indexEntry(c, 0).offset, BigInt(c.length), true),
      true,
      "INVALID_OFFSET",
      /entry 0 \('a\.bin'\) has a range outside/,
    ],
    [
      (c, v) => v.setBigUint64(indexEntry(c, 0).offset, 60n, true),
      true,
      "INVALID_OFFSET",
      /entry 0 \('a\.bin'\)/,
    ],
    [
      (c, v) => v.setBigUint64(indexEntry(c, 2).size, 0n, true),
      true,
      "INVALID_OFFSET",
      /entry 2 \('d\.bin'\)/,
    ],
    [name(2, "a"), true, "DUPLICATE_NAME", /entries 0 and 2 share the name 'a\.bin'/],
  ];
  for (const [patch, reseal, code, message] of cases) {
    await withServer(patched(bytes, patch, reseal), {}, async (url) => {
      await assert.rejects(verify(url), { code, message });
    });
  }
  const small = indexedArchive({ files: NONE_FILES, after: 0 });
  await withServer(small, {}, async (url) => {
    await assert.rejects(verify(url), { code: "ARCHIVE_TOO_SMALL" });
  });
});

test("verify names the first repeat in index order", async () => {
  const files = ["w.bin", "x.bin", "y.bin", "z.bin"].map((name) => ({
    name,
    data: new Uint8Array([1]),
    indexed: true,
  }));
  const renamed = patched(
    indexedArchive({ files }),
    (copy) => {
      ["b.bin", "a.bin", "b.bin", "a.bin"].forEach((text, i) =>
        copy.set(encoder.encode(text), indexEntry(copy, i).name),
      );
    },
    true,
  );
  await withServer(renamed, {}, async (url) => {
    await assert.rejects(verify(url), {
      code: "DUPLICATE_NAME",
      message: /entries 0 and 2 share the name 'b\.bin'/,
    });
  });
});

test("verify takes a whole-object 200 response and checks its arguments", async () => {
  const bytes = indexedArchive({ files: NONE_FILES });
  await withServer(bytes, { ignoreRange: true }, async (url, requests) => {
    assert.equal(await verify(url), undefined);
    assert.equal(requests.length, 1);
  });
  await assert.rejects(verify(42), TypeError);
  await assert.rejects(verify(""), /URL must not be empty/);
  await assert.rejects(verify("s3://bucket/a.zip"), /only http\(s\) URLs are supported/);
});
