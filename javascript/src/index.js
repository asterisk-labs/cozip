// Pure-JS reader for cozip archives over HTTP.

import { parquetReadObjects } from "hyparquet";
import { compressors } from "hyparquet-compressors";

const LFH_SIZE = 51;
const INDEX_HEADER_SIZE = 11;
const BOOTSTRAP_SIZE = 65536;
const LFH_MAGIC = 0x04034b50;
const INDEX_MAGIC = 0x50495a43; // 'CZIP' as little-endian uint32
const EXTRA_HEADER_ID = 0xca0c;
const COZIP_NAME = "__cozip__";
const METADATA_NAME = "__metadata__";

/**
 * @typedef {object} ReadOptions
 * @property {string[]} [columns]  Subset of extra columns from __metadata__ to include.
 * @property {boolean}  [gdalVsi]  Include the cozip:gdal_vsi column (default true).
 */

/**
 * Read a cozip archive's manifest over HTTP.
 *
 * @param {string} url
 * @param {ReadOptions} [opts]
 * @returns {Promise<object[]>} Rows with { name, offset, size, ...extras, "cozip:gdal_vsi"? }.
 */
export async function read(url, opts = {}) {
  if (!/^https?:\/\//i.test(url)) {
    throw new Error(`cozip: only http(s) URLs are supported, got: ${url}`);
  }
  const { columns, gdalVsi = true } = opts;

  // 1. Bootstrap the index from the first 64 KiB, extend if needed.
  const head = await fetchRange(url, 0, BOOTSTRAP_SIZE - 1);
  const indexEnd = LFH_SIZE + readIndexPayloadSize(head);
  const indexBuf =
    indexEnd > head.length
      ? concat(head, await fetchRange(url, head.length, indexEnd - 1))
      : head;

  // 2. Locate __metadata__ inside the index.
  const entries = parseIndex(indexBuf);
  const meta = entries.get(METADATA_NAME);
  if (!meta) {
    throw new Error("cozip: archive has no __metadata__ entry");
  }

  // 3. Fetch and parse the __metadata__ Parquet.
  const metaBytes = await fetchRange(url, meta.offset, meta.offset + meta.size - 1);
  const parquetCols = columns
    ? Array.from(new Set(["name", "offset", "size", ...columns]))
    : undefined;

  const file = /** @type {ArrayBuffer} */ (
    metaBytes.buffer.slice(
      metaBytes.byteOffset,
      metaBytes.byteOffset + metaBytes.byteLength,
    )
  );
  const rows = await parquetReadObjects({ file, columns: parquetCols, compressors });

  // 4. Inject the VSI path for GDAL consumers.
  if (gdalVsi) {
    for (const row of rows) {
      row["cozip:gdal_vsi"] = `/vsisubfile/${row.offset}_${row.size},/vsicurl/${url}`;
    }
  }

  return rows;
}

// internals :)

/**
 * @param {string} url
 * @param {number} start
 * @param {number} end
 * @returns {Promise<Uint8Array>}
 */
async function fetchRange(url, start, end) {
  const res = await fetch(url, { headers: { Range: `bytes=${start}-${end}` } });
  if (res.status !== 206 && res.status !== 200) {
    throw new Error(`cozip: HTTP ${res.status} ${res.statusText} for ${url}`);
  }
  return new Uint8Array(await res.arrayBuffer());
}

/**
 * @param {Uint8Array} a
 * @param {Uint8Array} b
 * @returns {Uint8Array}
 */
function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

/**
 * @param {Uint8Array} buf
 * @returns {number}
 */
function readIndexPayloadSize(buf) {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (v.getUint32(0, true) !== LFH_MAGIC) {
    throw new Error("cozip: byte 0 is not a ZIP local file header");
  }
  if (v.getUint16(26, true) !== 9 || v.getUint16(28, true) !== 12) {
    throw new Error("cozip: LFH does not match cozip layout");
  }
  const size = v.getUint32(18, true);
  if (size === 0) throw new Error("cozip: index payload size is zero");
  return size;
}

/**
 * @param {Uint8Array} buf
 * @returns {Map<string, { offset: number, size: number }>}
 */
function parseIndex(buf) {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const td = new TextDecoder();

  if (td.decode(buf.subarray(30, 39)) !== COZIP_NAME) {
    throw new Error("cozip: first ZIP entry is not __cozip__");
  }
  if (v.getUint16(39, true) !== EXTRA_HEADER_ID) {
    throw new Error("cozip: integrity extra field (0xCA0C) missing");
  }
  if (v.getUint32(LFH_SIZE, true) !== INDEX_MAGIC) {
    throw new Error("cozip: index magic is not CZIP");
  }

  const n = v.getUint32(LFH_SIZE + 7, true);
  let cur = LFH_SIZE + INDEX_HEADER_SIZE;

  const nameLens = new Array(n);
  for (let i = 0; i < n; i++) {
    nameLens[i] = v.getUint16(cur, true);
    cur += 2;
  }

  const names = new Array(n);
  for (let i = 0; i < n; i++) {
    names[i] = td.decode(buf.subarray(cur, cur + nameLens[i]));
    cur += nameLens[i];
  }

  const offsets = new Array(n);
  for (let i = 0; i < n; i++) {
    offsets[i] = Number(v.getBigUint64(cur, true));
    cur += 8;
  }

  const sizes = new Array(n);
  for (let i = 0; i < n; i++) {
    sizes[i] = Number(v.getBigUint64(cur, true));
    cur += 8;
  }

  const entries = new Map();
  for (let i = 0; i < n; i++) {
    entries.set(names[i], { offset: offsets[i], size: sizes[i] });
  }
  return entries;
}