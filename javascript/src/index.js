// Pure-JS reader for cozip archives over HTTP.

import { parquetReadObjects } from "hyparquet";
import { compressors } from "hyparquet-compressors";

const LFH_SIZE = 51;
const INDEX_HEADER_SIZE = 11;
const HASH_WINDOW_SIZE = 32768;
const MIN_ARCHIVE_SIZE = LFH_SIZE + HASH_WINDOW_SIZE;
const BOOTSTRAP_SIZE = 65536;
const LFH_MAGIC = 0x04034b50;
const INDEX_MAGIC = 0x50495a43; // 'CZIP' as little-endian uint32
const EXTRA_HEADER_ID = 0xca0c;
const FORBIDDEN_FLAGS = 0x2049;
const COZIP_NAME = "__cozip__";
const METADATA_NAME = "__metadata__";
const FORMAT_VERSION = 1;
const PROFILE_FLAT = 1;
const PROTECTED_LOCATION_COLUMNS = new Set(["cozip:location", "taco:location"]);
const FNV_OFFSET_BASIS = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;
const UINT64_MASK = 0xffffffffffffffffn;

/**
 * @typedef {object} ReadOptions
 * @property {string[]} [columns]  Subset of extra columns from __metadata__ to include.
 * @property {boolean}  [location]  Include the cozip:location column (default true).
 */

/**
 * Read a Flat-profile cozip archive's manifest over HTTP.
 *
 * @param {string} url
 * @param {ReadOptions} [opts]
 * @returns {Promise<object[]>} Rows with { name, offset, size, ...extras, "cozip:location"? }.
 */
export async function read(url, opts = {}) {
  if (typeof url !== "string") {
    throw new TypeError("cozip: URL must be a string");
  }
  if (url.length === 0) {
    throw new Error("cozip: URL must not be empty");
  }
  if (!/^[\x01-\x7f]+$/.test(url)) {
    throw new Error(
      "cozip: URL contains non-ASCII characters; only ASCII URLs are supported",
    );
  }
  if (!/^https?:\/\//i.test(url)) {
    throw new Error(`cozip: only http(s) URLs are supported, got: ${url}`);
  }
  if (opts === null || typeof opts !== "object" || Array.isArray(opts)) {
    throw new TypeError("cozip: options must be an object");
  }
  const { columns, location = true } = opts;
  if (
    columns !== undefined &&
    (!Array.isArray(columns) ||
      columns.some((column) => typeof column !== "string" || column.length === 0))
  ) {
    throw new TypeError("cozip: columns must be an array of non-empty strings");
  }
  if (typeof location !== "boolean") {
    throw new TypeError("cozip: location must be a boolean");
  }

  // 1. Bootstrap the index from the first 64 KiB, extend if needed.
  const first = await fetchRange(url, 0, BOOTSTRAP_SIZE - 1);
  const archiveSize = first.totalSize;
  if (archiveSize < MIN_ARCHIVE_SIZE) {
    throw new Error(
      `cozip ARCHIVE_TOO_SMALL: archive is ${archiveSize} bytes; minimum is ${MIN_ARCHIVE_SIZE}`,
    );
  }
  const head = first.bytes;
  const indexSize = readIndexPayloadSize(head);
  const indexEnd = LFH_SIZE + indexSize;
  if (indexEnd > archiveSize) {
    throw new Error("cozip TRUNCATED_INDEX: index payload extends beyond the archive");
  }
  const indexBuf =
    indexEnd > head.length
      ? concat(
          head,
          (await fetchArchiveRange(url, head.length, indexEnd - 1, archiveSize)).bytes,
        )
      : head;

  // 2. Parse and authenticate the fast-access path before using its offsets.
  const entries = parseIndex(indexBuf, archiveSize);
  await verifyIntegrityHash(url, indexBuf, indexSize, archiveSize);
  const meta = entries.get(METADATA_NAME);
  if (entries.size !== 1 || !meta) {
    throw new Error(
      "cozip MISSING_ENTRY: Flat profile requires exactly one priority entry named __metadata__",
    );
  }

  // 3. Fetch and parse the __metadata__ Parquet.
  const metaBytes = (
    await fetchArchiveRange(
      url,
      meta.offset,
      meta.offset + meta.size - 1,
      archiveSize,
    )
  ).bytes;
  const parquetCols = columns
    ? Array.from(
        new Set([
          "name",
          "offset",
          "size",
          // This column is produced below, not read from Parquet.
          ...columns.filter((column) => !PROTECTED_LOCATION_COLUMNS.has(column)),
        ]),
      )
    : undefined;

  const file = /** @type {ArrayBuffer} */ (
    metaBytes.buffer.slice(
      metaBytes.byteOffset,
      metaBytes.byteOffset + metaBytes.byteLength,
    )
  );
  const rows = await parquetReadObjects({ file, columns: parquetCols, compressors });

  // 4. Never trust reader-owned columns from Parquet; derive the location here.
  for (const row of rows) {
    for (const column of PROTECTED_LOCATION_COLUMNS) {
      delete row[column];
    }
    if (location) {
      row["cozip:location"] = `/vsisubfile/${row.offset}_${row.size},/vsicurl/${url}`;
    }
  }

  return rows;
}

// internals :)

/**
 * @param {string} url
 * @param {number} start
 * @param {number} end
 * @returns {Promise<{bytes: Uint8Array, totalSize: number}>}
 */
async function fetchRange(url, start, end) {
  const res = await fetch(url, { headers: { Range: `bytes=${start}-${end}` } });
  if (res.status !== 206 && res.status !== 200) {
    throw new Error(`cozip: HTTP ${res.status} ${res.statusText} for ${url}`);
  }
  const bytes = new Uint8Array(await res.arrayBuffer());
  if (res.status === 206) {
    const contentRange = res.headers.get("content-range");
    const match = contentRange?.match(/^bytes (\d+)-(\d+)\/(\d+)$/i);
    if (!match) {
      throw new Error("cozip: range response has no valid Content-Range header");
    }
    const responseStart = Number(match[1]);
    const responseEnd = Number(match[2]);
    const totalSize = Number(match[3]);
    if (
      !Number.isSafeInteger(responseStart) ||
      !Number.isSafeInteger(responseEnd) ||
      !Number.isSafeInteger(totalSize) ||
      responseStart !== start ||
      responseEnd !== Math.min(end, totalSize - 1) ||
      responseEnd < responseStart ||
      totalSize <= responseEnd
    ) {
      throw new Error(`cozip: invalid Content-Range header: ${contentRange}`);
    }
    const expected = responseEnd - responseStart + 1;
    if (bytes.length !== expected) {
      throw new Error(
        `cozip: range response has ${bytes.length} bytes, expected ${expected}`,
      );
    }
    return { bytes, totalSize };
  }

  // Some servers ignore Range and return the complete object with 200.
  if (start === 0) return { bytes, totalSize: bytes.length };
  if (bytes.length <= end) {
    throw new Error("cozip: server ignored Range but did not return the full object");
  }
  return { bytes: bytes.subarray(start, end + 1), totalSize: bytes.length };
}

/**
 * @param {string} url
 * @param {number} start
 * @param {number} end
 * @param {number} archiveSize
 * @returns {Promise<{bytes: Uint8Array, totalSize: number}>}
 */
async function fetchArchiveRange(url, start, end, archiveSize) {
  const result = await fetchRange(url, start, end);
  if (result.totalSize !== archiveSize) {
    throw new Error(
      `cozip: archive size changed while reading (${archiveSize} to ${result.totalSize} bytes)`,
    );
  }
  return result;
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
 * @param {Uint8Array} bytes
 * @param {bigint} hash
 * @returns {bigint}
 */
function fnv1a64(bytes, hash = FNV_OFFSET_BASIS) {
  for (const byte of bytes) {
    hash ^= BigInt(byte);
    hash = (hash * FNV_PRIME) & UINT64_MASK;
  }
  return hash;
}

/**
 * @param {string} url
 * @param {Uint8Array} head
 * @param {number} indexSize
 * @param {number} archiveSize
 */
async function verifyIntegrityHash(url, head, indexSize, archiveSize) {
  const view = new DataView(head.buffer, head.byteOffset, head.byteLength);
  const stored = view.getBigUint64(43, true);
  const indexEnd = LFH_SIZE + indexSize;
  let hash = fnv1a64(head.subarray(LFH_SIZE, indexEnd));

  const suffixStart = archiveSize - HASH_WINDOW_SIZE;
  let suffix;
  if (archiveSize <= head.length) {
    suffix = head.subarray(suffixStart, archiveSize);
  } else if (suffixStart < head.length) {
    const cached = head.subarray(suffixStart);
    const remainder = (
      await fetchArchiveRange(url, head.length, archiveSize - 1, archiveSize)
    ).bytes;
    suffix = concat(cached, remainder);
  } else {
    suffix = (
      await fetchArchiveRange(url, suffixStart, archiveSize - 1, archiveSize)
    ).bytes;
  }

  const remainingStart = Math.max(suffixStart, indexEnd);
  if (remainingStart < archiveSize) {
    hash = fnv1a64(suffix.subarray(remainingStart - suffixStart), hash);
  }
  if (hash !== stored) {
    throw new Error(
      "cozip HASH_MISMATCH: integrity hash does not match the index and archive suffix",
    );
  }
}

/**
 * @param {Uint8Array} buf
 * @returns {number}
 */
function readIndexPayloadSize(buf) {
  if (buf.length < LFH_SIZE) {
    throw new Error("cozip: response is shorter than the 51-byte index header");
  }
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (v.getUint32(0, true) !== LFH_MAGIC) {
    throw new Error("cozip: byte 0 is not a ZIP local file header");
  }
  if ((v.getUint16(6, true) & FORBIDDEN_FLAGS) !== 0) {
    throw new Error("cozip: index local file header has forbidden flags");
  }
  if (v.getUint16(8, true) !== 0) {
    throw new Error("cozip: index entry does not use STORE compression");
  }
  if (v.getUint16(26, true) !== 9 || v.getUint16(28, true) !== 12) {
    throw new Error("cozip: LFH does not match cozip layout");
  }
  const size = v.getUint32(18, true);
  if (size === 0 || size === 0xffffffff || v.getUint32(22, true) !== size) {
    throw new Error("cozip: index entry has invalid size fields");
  }
  if (size < INDEX_HEADER_SIZE) {
    throw new Error("cozip: index payload is shorter than its 11-byte header");
  }
  return size;
}

/**
 * @param {Uint8Array} buf
 * @param {number} archiveSize
 * @returns {Map<string, { offset: number, size: number }>}
 */
function parseIndex(buf, archiveSize) {
  if (buf.length < LFH_SIZE + INDEX_HEADER_SIZE) {
    throw new Error("cozip: truncated index payload");
  }
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const td = new TextDecoder();

  if (td.decode(buf.subarray(30, 39)) !== COZIP_NAME) {
    throw new Error("cozip: first ZIP entry is not __cozip__");
  }
  if (v.getUint16(39, true) !== EXTRA_HEADER_ID) {
    throw new Error("cozip: integrity extra field (0xCA0C) missing");
  }
  if (v.getUint16(41, true) !== 8) {
    throw new Error("cozip: integrity extra field must contain eight bytes");
  }
  if (v.getUint32(LFH_SIZE, true) !== INDEX_MAGIC) {
    throw new Error("cozip: index magic is not CZIP");
  }
  const version = v.getUint16(LFH_SIZE + 4, true);
  if (version > FORMAT_VERSION) {
    throw new Error(`cozip: unsupported format version ${version}`);
  }
  const profile = v.getUint8(LFH_SIZE + 6);
  if (profile !== PROFILE_FLAT) {
    throw new Error(
      `cozip UNKNOWN_PROFILE: read() requires Flat profile 1, got profile ${profile}`,
    );
  }

  const n = v.getUint32(LFH_SIZE + 7, true);
  const payloadSize = v.getUint32(18, true);
  const payloadEnd = LFH_SIZE + payloadSize;
  if (payloadEnd > buf.length) {
    throw new Error("cozip: index payload extends beyond the response");
  }
  if (n > Math.floor((payloadSize - INDEX_HEADER_SIZE) / 18)) {
    throw new Error("cozip: index entry count does not fit in its payload");
  }
  let cur = LFH_SIZE + INDEX_HEADER_SIZE;

  const nameLens = new Array(n);
  let namesSize = 0;
  for (let i = 0; i < n; i++) {
    nameLens[i] = v.getUint16(cur, true);
    namesSize += nameLens[i];
    cur += 2;
  }
  if (INDEX_HEADER_SIZE + n * 18 + namesSize !== payloadSize) {
    throw new Error("cozip: index sections do not match its declared size");
  }

  const names = new Array(n);
  const uniqueNames = new Set();
  for (let i = 0; i < n; i++) {
    const bytes = buf.subarray(cur, cur + nameLens[i]);
    names[i] = td.decode(bytes);
    validateArchiveName(bytes, names[i], i);
    if (uniqueNames.has(names[i])) {
      throw new Error(`cozip: index contains duplicate name ${JSON.stringify(names[i])}`);
    }
    uniqueNames.add(names[i]);
    cur += nameLens[i];
  }

  const offsets = new Array(n);
  for (let i = 0; i < n; i++) {
    offsets[i] = readSafeUint64(v, cur, `offset for ${JSON.stringify(names[i])}`);
    cur += 8;
  }

  const sizes = new Array(n);
  for (let i = 0; i < n; i++) {
    sizes[i] = readSafeUint64(v, cur, `size for ${JSON.stringify(names[i])}`);
    if (sizes[i] === 0) {
      throw new Error(`cozip: index entry ${JSON.stringify(names[i])} has a zero-byte payload`);
    }
    if (!Number.isSafeInteger(offsets[i] + sizes[i])) {
      throw new Error(`cozip: offset and size overflow for ${JSON.stringify(names[i])}`);
    }
    if (offsets[i] + sizes[i] > archiveSize) {
      throw new Error(
        `cozip INVALID_OFFSET: index entry ${JSON.stringify(names[i])} extends beyond the archive`,
      );
    }
    cur += 8;
  }

  const entries = new Map();
  for (let i = 0; i < n; i++) {
    entries.set(names[i], { offset: offsets[i], size: sizes[i] });
  }
  return entries;
}

/**
 * @param {DataView} view
 * @param {number} offset
 * @param {string} context
 * @returns {number}
 */
function readSafeUint64(view, offset, context) {
  const value = view.getBigUint64(offset, true);
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new Error(`cozip: ${context} exceeds JavaScript's safe integer range`);
  }
  return Number(value);
}

/**
 * @param {Uint8Array} bytes
 * @param {string} name
 * @param {number} index
 */
function validateArchiveName(bytes, name, index) {
  if (bytes.length === 0) {
    throw new Error(`cozip: index entry ${index} has an empty name`);
  }
  if (bytes.some((byte) => byte === 0 || byte >= 0x80)) {
    throw new Error(
      `cozip: index entry ${index} name contains a byte outside ASCII 0x01-0x7F`,
    );
  }
  if (
    name.startsWith("/") ||
    /^[A-Za-z]:/.test(name) ||
    name.endsWith("/") ||
    name.includes("\\") ||
    name.split("/").some((part) => part === "." || part === "..")
  ) {
    throw new Error(`cozip: index entry ${index} has an invalid archive name`);
  }
  if (name === COZIP_NAME) {
    throw new Error("cozip: index must not list the reserved __cozip__ entry");
  }
  if (name === "__cozip_padding__") {
    throw new Error(
      "cozip: index must not list the reserved __cozip_padding__ entry",
    );
  }
}
