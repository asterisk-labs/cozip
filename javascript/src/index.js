// Pure-JS reader for cozip archives over HTTP.

import { parquetMetadata, parquetReadObjects, parquetSchema } from "hyparquet";
import { compressors } from "hyparquet-compressors";

const LFH_SIZE = 51;
const INDEX_HEADER_SIZE = 11;
const HASH_WINDOW_SIZE = 32768;
const MIN_ARCHIVE_SIZE = LFH_SIZE + HASH_WINDOW_SIZE;
// The first request; a Flat index always lies inside it.
const BOOTSTRAP_SIZE = 65536;
const LFH_MAGIC = 0x04034b50;
const EOCD_MAGIC = 0x06054b50;
const EOCD_SIZE = 22;
const INDEX_MAGIC = 0x50495a43; // 'CZIP' as little-endian uint32
const EXTRA_HEADER_ID = 0xca0c;
const ZIP64_EXTRA_ID = 0x0001;
const FORBIDDEN_FLAGS = 0x2049;
const COZIP_NAME = "__cozip__";
const METADATA_NAME = "__metadata__";
const FORMAT_VERSION = 1;
const PROFILE_FLAT = 1;
// A Flat index lists one name, __metadata__: 11 + 2 + 12 + 8 + 8 bytes.
const FLAT_INDEX_SIZE = INDEX_HEADER_SIZE + 2 + METADATA_NAME.length + 16;
const INDEX_END = LFH_SIZE + FLAT_INDEX_SIZE;
// The local file header of __metadata__ before its extra field.
const METADATA_HEADER_SIZE = 30 + METADATA_NAME.length;
// Largest extra field accepted on that header, as other writers may add one.
const METADATA_EXTRA_MAX = 1024;
// The manifest payload cannot start before the index and its own header end.
const FIRST_PAYLOAD = INDEX_END + METADATA_HEADER_SIZE;
// Reader-owned columns (spec 12.3), matched without case. Stored copies are
// never read, and naming one in `columns` is not an error.
const READER_COLUMNS = new Set(["cozip:location", "taco:location", "cozip:gdal_vsi"]);
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
 * Malformed archives raise an error with a `code` property, such as
 * `HASH_MISMATCH` or `INVALID_INDEX`.
 *
 * @param {string} url
 * @param {ReadOptions} [opts]
 * @returns {Promise<object[]>} Rows with { name, offset, size, ...extras, "cozip:location"? }.
 */
export async function read(url, opts = {}) {
  checkUrl(url);
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

  // 1. Check the fixed header and the Flat index from the first response.
  const archive = await openArchive(url);
  checkHeader(archive.head, archive.size);
  checkFlatIndex(archive.head);

  // 2. Authenticate the index and the archive tail before using its offsets.
  const suffix = await readSuffix(archive);
  verifyIntegrityHash(archive.head.subarray(LFH_SIZE, INDEX_END), archive.head, suffix, archive.size);
  checkEndRecord(suffix);
  const meta = readFlatEntry(archive.head, archive.size);

  // 3. Fetch and parse the __metadata__ Parquet.
  const metaBytes = await readManifest(archive, meta);
  const file = /** @type {ArrayBuffer} */ (
    metaBytes.buffer.slice(
      metaBytes.byteOffset,
      metaBytes.byteOffset + metaBytes.byteLength,
    )
  );
  const metadata = parquetMetadata(file);
  const stored = parquetSchema(metadata).children.map((child) => child.element.name);
  let parquetCols = stored.filter((column) => !isReaderColumn(column));
  if (columns) {
    const requested = columns.filter((column) => !isReaderColumn(column));
    const known = new Set(stored);
    const unknown = Array.from(new Set(requested.filter((column) => !known.has(column))));
    if (unknown.length > 0) {
      const names = unknown.map((column) => JSON.stringify(column)).join(", ");
      throw new Error(`cozip: columns not in the manifest: ${names}`);
    }
    parquetCols = Array.from(new Set(["name", "offset", "size", ...requested]));
  }
  const rows = await parquetReadObjects({
    file,
    metadata,
    columns: parquetCols,
    compressors,
  });

  // 4. Stored reader-owned columns were never read; derive the location here.
  if (location) {
    for (const row of rows) {
      row["cozip:location"] = `/vsisubfile/${row.offset}_${row.size},/vsicurl/${url}`;
    }
  }

  return rows;
}

/**
 * Check a cozip archive's access path, whatever its profile.
 *
 * Checks the header, index, indexed names and ranges, integrity hash and ZIP
 * end record (spec 8.5 and 7). It does not parse the Central Directory,
 * check payloads or enforce profile rules. A failed check rejects with a
 * `code` such as `HASH_MISMATCH`.
 *
 * @param {string} url
 * @returns {Promise<void>}
 */
export async function verify(url) {
  checkUrl(url);
  const archive = await openArchive(url);
  const indexSize = checkHeader(archive.head, archive.size);
  const indexEnd = LFH_SIZE + indexSize;
  // Spec 8.5 step 3 reads the whole index. Only an index of another profile
  // than Flat runs past the first response; the rest extends the head.
  if (indexEnd > archive.head.length) {
    const rest = await fetchArchiveRange(archive, archive.head.length, indexEnd - 1);
    archive.head = concat(archive.head, rest);
  }
  const index = archive.head.subarray(LFH_SIZE, indexEnd);
  const suffix = await readSuffix(archive);
  verifyIntegrityHash(index, archive.head, suffix, archive.size);
  checkEndRecord(suffix);
  checkIndex(index, archive.size);
}

/**
 * Arguments shared by read and verify: an ASCII http(s) URL.
 *
 * @param {unknown} url
 */
function checkUrl(url) {
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
    throw new Error(`cozip: only http(s) URLs are supported, got: ${redactUrl(url)}`);
  }
}

/** State for one read. Later requests use If-Match when an ETag is available. */
class Archive {
  /**
   * @param {string} url
   * @param {string} shown  The URL without credentials, for messages.
   * @param {number} size
   * @param {string | null} etag  Strong ETag of the first response.
   * @param {Uint8Array} head  Archive bytes from offset 0.
   */
  constructor(url, shown, size, etag, head) {
    this.url = url;
    this.shown = shown;
    this.size = size;
    this.etag = etag;
    this.head = head;
    /** @type {Uint8Array | null} The hash suffix, when apart from head. */
    this.tail = null;
  }
}

/**
 * @param {string} code  Spec section 9 name or INVALID_INDEX.
 * @param {string} message
 * @returns {Error & { code: string }}
 */
function archiveError(code, message) {
  return Object.assign(new Error(`cozip ${code}: ${message}`), { code });
}

/**
 * @param {string} column
 * @returns {boolean}
 */
function isReaderColumn(column) {
  return READER_COLUMNS.has(column.toLowerCase());
}

/**
 * The URL without userinfo, query or fragment, for messages. Locations keep
 * the URL as given.
 *
 * @param {string} url
 * @returns {string}
 */
function redactUrl(url) {
  let parsed = null;
  try {
    parsed = new URL(url);
  } catch {
    // Handled below.
  }
  if (parsed && parsed.host) {
    const user = parsed.username || parsed.password ? "<redacted>@" : "";
    const query = parsed.search ? "?<redacted>" : "";
    const fragment = parsed.hash ? "#<redacted>" : "";
    return `${parsed.protocol}//${user}${parsed.host}${parsed.pathname}${query}${fragment}`;
  }
  // Not parseable: cut at the first ? or #, and treat everything up to the
  // last @ as userinfo.
  const cut = url.search(/[?#]/);
  const base = cut < 0 ? url : url.slice(0, cut);
  const secret = cut < 0 ? "" : `${url[cut]}<redacted>`;
  const scheme = base.indexOf("://");
  const at = base.lastIndexOf("@");
  if (scheme >= 0 && at > scheme) {
    return `${base.slice(0, scheme + 3)}<redacted>${base.slice(at)}${secret}`;
  }
  return base + secret;
}

/**
 * The message of a failed fetch and its causes, with URLs redacted.
 *
 * @param {unknown} error
 * @param {string} url
 * @param {string} shown
 * @returns {string}
 */
function errorText(error, url, shown) {
  const parts = [];
  for (let e = error; e instanceof Error && parts.length < 3; e = e.cause) {
    if (e.message) parts.push(e.message);
  }
  const text = parts.length > 0 ? parts.join(": ") : String(error);
  const token = "\u0000url\u0000";
  return text
    .split(url)
    .join(token)
    .replace(/[a-z][a-z0-9+.-]*:\/\/[^\s"'<>\u0000]+/gi, (match) => redactUrl(match))
    .split(token)
    .join(shown);
}

/**
 * @param {string} url
 * @param {string} shown
 * @param {number} start
 * @param {number} end
 * @param {string | null} etag  Sent as If-Match when set.
 * @returns {Promise<{bytes: Uint8Array, totalSize: number, etag: string | null}>}
 */
async function fetchRange(url, shown, start, end, etag) {
  /** @type {Record<string, string>} */
  const headers = { Range: `bytes=${start}-${end}` };
  if (etag) headers["If-Match"] = etag;
  let res;
  let bytes = new Uint8Array(0);
  try {
    res = await fetch(url, { headers });
    if (res.status === 206 || res.status === 200) {
      bytes = new Uint8Array(await res.arrayBuffer());
    } else {
      await res.body?.cancel();
    }
  } catch (error) {
    throw new Error(`cozip: could not fetch ${shown}: ${errorText(error, url, shown)}`);
  }
  if (res.status === 412 && etag) {
    throw new Error(`cozip: archive changed while reading (HTTP 412 for ${shown})`);
  }
  if (res.status !== 206 && res.status !== 200) {
    throw new Error(`cozip: HTTP ${res.status} ${res.statusText} for ${shown}`);
  }
  const responseTag = res.headers.get("etag");
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
    return { bytes, totalSize, etag: responseTag };
  }

  // Some servers ignore Range and return the complete object with 200. The
  // first request keeps all of it.
  if (start === 0) return { bytes, totalSize: bytes.length, etag: responseTag };
  if (bytes.length <= end) {
    throw new Error("cozip: server ignored Range but did not return the full object");
  }
  return {
    bytes: bytes.subarray(start, end + 1),
    totalSize: bytes.length,
    etag: responseTag,
  };
}

/**
 * The first request: the head, the archive size and the version to pin.
 *
 * @param {string} url
 * @returns {Promise<Archive>}
 */
async function openArchive(url) {
  const shown = redactUrl(url);
  const first = await fetchRange(url, shown, 0, BOOTSTRAP_SIZE - 1, null);
  // A weak ETag cannot pin byte ranges; CORS may also hide the header.
  const etag = first.etag && !/^W\//i.test(first.etag) ? first.etag : null;
  return new Archive(url, shown, first.totalSize, etag, first.bytes);
}

/**
 * A later request, conditional on the first ETag when available.
 *
 * @param {Archive} archive
 * @param {number} start
 * @param {number} end
 * @returns {Promise<Uint8Array>}
 */
async function fetchArchiveRange(archive, start, end) {
  const result = await fetchRange(archive.url, archive.shown, start, end, archive.etag);
  const tag = result.etag?.replace(/^W\//i, "");
  if (archive.etag && tag && tag !== archive.etag) {
    throw new Error(
      `cozip: archive changed while reading (ETag ${archive.etag} is now ${result.etag})`,
    );
  }
  if (result.totalSize !== archive.size) {
    throw new Error(
      `cozip: archive size changed while reading (${archive.size} to ${result.totalSize} bytes)`,
    );
  }
  return result.bytes;
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
 * @returns {DataView}
 */
function viewOf(buf) {
  return new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
}

/**
 * @param {Uint8Array} buf
 * @param {number} at
 * @param {string} text  ASCII.
 * @returns {boolean}
 */
function hasAscii(buf, at, text) {
  for (let i = 0; i < text.length; i++) {
    if (buf[at + i] !== text.charCodeAt(i)) return false;
  }
  return true;
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
 * Spec 8.5 step 1, the archive size and the index header (7.1), all from the
 * first response. Returns the index payload size.
 *
 * @param {Uint8Array} head
 * @param {number} archiveSize
 * @returns {number}
 */
function checkHeader(head, archiveSize) {
  if (archiveSize < LFH_SIZE) {
    throw archiveError(
      "ARCHIVE_TOO_SMALL",
      `archive is ${archiveSize} bytes; minimum is ${MIN_ARCHIVE_SIZE}`,
    );
  }
  const v = viewOf(head);
  if (v.getUint32(0, true) !== LFH_MAGIC) {
    throw archiveError("INVALID_LFH", "byte 0 is not a ZIP local file header");
  }
  if ((v.getUint16(6, true) & FORBIDDEN_FLAGS) !== 0) {
    throw archiveError("INVALID_LFH", "index local file header has forbidden flags");
  }
  if (v.getUint16(8, true) !== 0) {
    throw archiveError("INVALID_LFH", "index entry does not use STORE compression");
  }
  const size = v.getUint32(18, true);
  if (size === 0 || size === 0xffffffff || v.getUint32(22, true) !== size) {
    throw archiveError("INVALID_LFH", "index entry sizes are not an equal, non-zero ZIP32 pair");
  }
  if (v.getUint16(26, true) !== COZIP_NAME.length || v.getUint16(28, true) !== 12) {
    throw archiveError("INVALID_LFH", "local file header does not match cozip layout");
  }
  if (!hasAscii(head, 30, COZIP_NAME)) {
    throw archiveError("INVALID_LFH", "first ZIP entry is not __cozip__");
  }
  if (v.getUint16(39, true) !== EXTRA_HEADER_ID || v.getUint16(41, true) !== 8) {
    throw archiveError("INVALID_LFH", "integrity extra field (0xCA0C, 8 bytes) is missing");
  }
  if (size < INDEX_HEADER_SIZE || LFH_SIZE + size > archiveSize) {
    throw archiveError(
      "TRUNCATED_INDEX",
      `index payload of ${size} bytes does not fit its header or the archive`,
    );
  }
  if (archiveSize < MIN_ARCHIVE_SIZE) {
    throw archiveError(
      "ARCHIVE_TOO_SMALL",
      `archive is ${archiveSize} bytes; minimum is ${MIN_ARCHIVE_SIZE}`,
    );
  }
  if (v.getUint32(LFH_SIZE, true) !== INDEX_MAGIC) {
    throw archiveError("INVALID_MAGIC", "index magic is not CZIP");
  }
  const version = v.getUint16(LFH_SIZE + 4, true);
  if (version === 0 || version > FORMAT_VERSION) {
    throw archiveError("UNSUPPORTED_VERSION", `unsupported format version ${version}`);
  }
  return size;
}

/**
 * The Flat index shape (13.2), from the first response and before anything
 * else is fetched, so an archive of another profile costs no more requests.
 *
 * @param {Uint8Array} head
 */
function checkFlatIndex(head) {
  const v = viewOf(head);
  const size = v.getUint32(18, true);
  const profile = v.getUint8(LFH_SIZE + 6);
  if (profile !== PROFILE_FLAT) {
    throw archiveError(
      "UNKNOWN_PROFILE",
      `read() requires Flat profile 1, got profile ${profile}`,
    );
  }
  const count = v.getUint32(LFH_SIZE + 7, true);
  if (
    count !== 1 ||
    size !== FLAT_INDEX_SIZE ||
    v.getUint16(LFH_SIZE + INDEX_HEADER_SIZE, true) !== METADATA_NAME.length
  ) {
    throw archiveError(
      "INVALID_INDEX",
      `Flat profile requires an index listing only __metadata__ in ${FLAT_INDEX_SIZE} bytes; ` +
        `got ${count} entries in ${size} bytes`,
    );
  }
}

/**
 * Reads the hash suffix, reusing the head where it already holds it.
 *
 * @param {Archive} archive
 * @returns {Promise<Uint8Array>}
 */
async function readSuffix(archive) {
  const { head, size } = archive;
  const suffixStart = size - HASH_WINDOW_SIZE;
  if (head.length < size) {
    if (suffixStart > head.length) {
      archive.tail = await fetchArchiveRange(archive, suffixStart, size - 1);
      return archive.tail;
    }
    // The suffix meets the head, so one range completes the archive.
    archive.head = concat(head, await fetchArchiveRange(archive, head.length, size - 1));
  }
  return archive.head.subarray(suffixStart, size);
}

/**
 * Spec 8.2 and 8.3: FNV-1a 64 over the index payload and the last 32 KiB.
 *
 * @param {Uint8Array} index  The whole index payload.
 * @param {Uint8Array} head  Archive bytes from offset 0, for the stored hash.
 * @param {Uint8Array} suffix  The last HASH_WINDOW_SIZE bytes.
 * @param {number} archiveSize
 */
function verifyIntegrityHash(index, head, suffix, archiveSize) {
  const stored = viewOf(head).getBigUint64(43, true);
  let hash = fnv1a64(index);
  // Bytes shared by the index and the suffix are hashed once.
  const suffixStart = archiveSize - HASH_WINDOW_SIZE;
  const indexEnd = LFH_SIZE + index.length;
  hash = fnv1a64(suffix.subarray(Math.max(0, indexEnd - suffixStart)), hash);
  if (hash !== stored) {
    throw archiveError(
      "HASH_MISMATCH",
      "integrity hash does not match the index and archive suffix",
    );
  }
}

/**
 * Spec 5.1.11: the comment-free end record is the last 22 bytes.
 *
 * @param {Uint8Array} suffix
 */
function checkEndRecord(suffix) {
  const v = viewOf(suffix);
  const at = suffix.length - EOCD_SIZE;
  if (v.getUint32(at, true) !== EOCD_MAGIC || v.getUint16(at + 20, true) !== 0) {
    throw archiveError(
      "INVALID_ZIP_STRUCTURE",
      "archive does not end with a comment-free end of central directory record",
    );
  }
}

// A name in a message is cut to this many characters.
const SHOWN_NAME_MAX = 120;

/**
 * Spec 5.1.8 and 5.3.1-5.3.5: why a name is not a valid archive name, or
 * null when it is. The same rules as the C writer and reader.
 *
 * @param {Uint8Array} name
 * @returns {string | null}
 */
function nameProblem(name) {
  if (name.length === 0) return "is empty";
  if (name.some((byte) => byte === 0 || byte >= 0x80)) {
    return "contains non-ASCII bytes; cozip archive names must be ASCII";
  }
  const text = String.fromCharCode(...name);
  if (text.startsWith("/")) return "must be relative, not start with '/'";
  if (/^[A-Za-z]:/.test(text)) return "must not have a drive-letter prefix";
  if (text.endsWith("/")) return "ends with '/'; explicit directory entries are not allowed";
  if (text.includes("\\")) return "contains '\\'; use '/' as the separator";
  if (text.split("/").some((part) => part === "." || part === "..")) {
    return "contains a '.' or '..' path component";
  }
  return null;
}

/**
 * Spec 7.2 to 7.5 and 5.3: the five sections fill the payload exactly, every
 * name is a valid archive name other than __cozip__ and __cozip_padding__,
 * no name repeats, and every range is non-empty and lies inside the archive
 * after the index and the entry's own local header.
 *
 * @param {Uint8Array} index  The whole index payload.
 * @param {number} archiveSize
 */
function checkIndex(index, archiveSize) {
  const v = viewOf(index);
  const size = index.length;
  const count = v.getUint32(7, true);
  let needed = INDEX_HEADER_SIZE + 18 * count;
  if (needed > size) {
    throw archiveError(
      "TRUNCATED_INDEX",
      `index payload of ${size} bytes cannot hold ${count} entries`,
    );
  }
  const lengths = [];
  for (let i = 0; i < count; i++) {
    lengths.push(v.getUint16(INDEX_HEADER_SIZE + 2 * i, true));
  }
  needed += lengths.reduce((total, length) => total + length, 0);
  if (needed > size) {
    throw archiveError(
      "TRUNCATED_INDEX",
      `index payload of ${size} bytes is shorter than its ${count} entries need (${needed} bytes)`,
    );
  }
  if (needed < size) {
    throw archiveError(
      "INVALID_INDEX",
      `index payload of ${size} bytes holds only ${needed} bytes of sections`,
    );
  }

  // The sections fill the payload, so offsets and sizes are its last
  // 16 bytes per entry.
  const offsets = size - 16 * count;
  const indexEnd = LFH_SIZE + size;
  const seen = new Map();
  /** @type {[number, number, string] | null} */
  let repeat = null;
  let at = INDEX_HEADER_SIZE + 2 * count;
  for (let i = 0; i < count; i++) {
    const name = index.subarray(at, at + lengths[i]);
    at += lengths[i];
    const problem = nameProblem(name);
    if (problem) {
      throw archiveError("INVALID_NAME", `index entry ${i} name ${problem}`);
    }
    const text = String.fromCharCode(...name);
    const shown = text.slice(0, SHOWN_NAME_MAX);
    if (text === COZIP_NAME || text === "__cozip_padding__") {
      throw archiveError(
        "INVALID_NAME",
        `index entry ${i} uses the reserved name '${shown}', which is never indexed`,
      );
    }
    const offset = v.getBigUint64(offsets + 8 * i, true);
    const length = v.getBigUint64(offsets + 8 * count + 8 * i, true);
    const total = BigInt(archiveSize);
    if (
      length === 0n ||
      offset < BigInt(indexEnd + 30 + name.length) ||
      offset > total ||
      length > total - offset
    ) {
      throw archiveError(
        "INVALID_OFFSET",
        `index entry ${i} ('${shown}') has a range outside the archive`,
      );
    }
    // Names and ranges come first; then the first name that repeats.
    const first = seen.get(text);
    if (first === undefined) seen.set(text, i);
    else if (repeat === null) repeat = [first, i, shown];
  }
  if (repeat) {
    const [first, second, shown] = repeat;
    throw archiveError(
      "DUPLICATE_NAME",
      `index entries ${first} and ${second} share the name '${shown}'`,
    );
  }
}

/**
 * The one entry of a Flat index, already checked by checkFlatIndex.
 *
 * @param {Uint8Array} head
 * @param {number} archiveSize
 * @returns {{ offset: number, size: number }}
 */
function readFlatEntry(head, archiveSize) {
  const nameAt = LFH_SIZE + INDEX_HEADER_SIZE + 2;
  if (!hasAscii(head, nameAt, METADATA_NAME)) {
    throw archiveError("INVALID_INDEX", "index has no entry named __metadata__");
  }
  const v = viewOf(head);
  const offset = v.getBigUint64(nameAt + METADATA_NAME.length, true);
  const size = v.getBigUint64(nameAt + METADATA_NAME.length + 8, true);
  if (size === 0n) {
    throw archiveError("INVALID_OFFSET", "index entry __metadata__ has a zero-byte payload");
  }
  if (offset < BigInt(FIRST_PAYLOAD)) {
    throw archiveError(
      "INVALID_OFFSET",
      `index entry __metadata__ starts at ${offset}, inside the index or its own local header`,
    );
  }
  if (offset + size > BigInt(archiveSize)) {
    throw archiveError(
      "INVALID_OFFSET",
      "index entry __metadata__ extends beyond the archive",
    );
  }
  return { offset: Number(offset), size: Number(size) };
}

/**
 * Returns the manifest bytes, from memory when the head or the suffix holds
 * them, else from one more request that starts a little earlier so the
 * manifest's local file header comes along.
 *
 * @param {Archive} archive
 * @param {{ offset: number, size: number }} meta
 * @returns {Promise<Uint8Array>}
 */
async function readManifest(archive, { offset, size }) {
  const end = offset + size;
  const { head, tail } = archive;
  if (end <= head.length) {
    checkManifestHeader(head, 0, offset, size, "");
    return head.subarray(offset, end);
  }
  const tailStart = archive.size - HASH_WINDOW_SIZE;
  if (tail && offset >= tailStart) {
    checkManifestHeader(tail, tailStart, offset, size, "");
    return tail.subarray(offset - tailStart, end - tailStart);
  }
  const start = Math.max(INDEX_END, offset - METADATA_HEADER_SIZE - METADATA_EXTRA_MAX);
  const bytes = await fetchArchiveRange(archive, start, end - 1);
  // Without a strong ETag, the header check catches some replacements.
  checkManifestHeader(
    bytes,
    start,
    offset,
    size,
    "; the archive may have changed while it was read",
  );
  return bytes.subarray(offset - start);
}

/**
 * Spec 9 MISSING_ENTRY: the local file header of __metadata__ must end at
 * its indexed offset, use STORE and record its indexed size. buf holds the
 * archive bytes from bufStart; when it does not reach back far enough to
 * decide, the check passes.
 *
 * @param {Uint8Array} buf
 * @param {number} bufStart
 * @param {number} offset
 * @param {number} size
 * @param {string} hint
 */
function checkManifestHeader(buf, bufStart, offset, size, hint) {
  const v = viewOf(buf);
  for (let extra = 0; extra <= METADATA_EXTRA_MAX; extra++) {
    const at = offset - METADATA_HEADER_SIZE - extra;
    if (at < INDEX_END) break;
    if (at < bufStart) return;
    const p = at - bufStart;
    if (
      v.getUint32(p, true) !== LFH_MAGIC ||
      v.getUint16(p + 26, true) !== METADATA_NAME.length ||
      v.getUint16(p + 28, true) !== extra ||
      !hasAscii(buf, p + 30, METADATA_NAME)
    ) {
      continue;
    }
    if (manifestHeaderMatches(v, p, extra, size)) return;
    break;
  }
  throw archiveError(
    "MISSING_ENTRY",
    `the local file header before __metadata__ does not match the index${hint}`,
  );
}

/**
 * @param {DataView} v
 * @param {number} p  Start of the local file header in v.
 * @param {number} extra  Its extra field length.
 * @param {number} size
 * @returns {boolean}
 */
function manifestHeaderMatches(v, p, extra, size) {
  if ((v.getUint16(p + 6, true) & FORBIDDEN_FLAGS) !== 0 || v.getUint16(p + 8, true) !== 0) {
    return false;
  }
  const compressed = v.getUint32(p + 18, true);
  const uncompressed = v.getUint32(p + 22, true);
  if (compressed !== 0xffffffff || uncompressed !== 0xffffffff) {
    return compressed === uncompressed && compressed === size;
  }
  // ZIP64: the 0x0001 extra field carries both sizes.
  const extraEnd = p + METADATA_HEADER_SIZE + extra;
  for (let x = p + METADATA_HEADER_SIZE; x + 4 <= extraEnd; ) {
    const id = v.getUint16(x, true);
    const length = v.getUint16(x + 2, true);
    if (x + 4 + length > extraEnd) return false;
    if (id === ZIP64_EXTRA_ID && length >= 16) {
      return (
        v.getBigUint64(x + 4, true) === BigInt(size) &&
        v.getBigUint64(x + 12, true) === BigInt(size)
      );
    }
    x += 4 + length;
  }
  return false;
}
