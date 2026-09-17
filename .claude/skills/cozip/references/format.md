# On-disk format and conformance

Sources: `SPEC.md` (version 0.1.0, dated 2026-09-09), `core/cozip.h`, `core/cozip.c`,
`scripts/check_cozip.py` in this skill. Offsets and sizes below were measured on archives
written by cozip 2026.9.9. `SPEC.md` is normative; when this file and the spec disagree,
the spec wins. Comments in `core/cozip.h` still call the rules "cozip 1.1"; the spec's
version line reads 0.1.0 since 2026-09-09, and the binary format version is 1 in both.

## Contents

1. Layout
2. The `__cozip__` local file header
3. Index payload
4. Integrity hash
5. Rules for every ZIP entry
6. Names
7. Size floor and padding
8. ZIP64
9. Profiles
10. Error codes
11. Validating an archive
12. Worked offsets

## 1. Layout

```text
byte 0    LFH of __cozip__ (51 bytes, hash extra included)
byte 51   index payload: CZIP header, name lengths, names, offsets, sizes
          [LFH][payload] for every other entry, in writer order
          Central Directory
          [ZIP64 end of central directory record and locator, when needed]
          End of Central Directory, comment length 0, last 22 bytes of the file
```

Nothing precedes byte 0: no self-extractor stub, no spanning marker. The archive is a
single-segment ZIP, readable by `unzip`, `zipfile.ZipFile` and friends. A cozip is
recognised by the `__cozip__` entry at byte 0 and its profile by the index profile byte,
never by the file extension, which is always `.zip` (MIME `application/zip`).

## 2. The `__cozip__` local file header

All integers are little endian.

| Bytes | Field | Required value |
| --- | --- | --- |
| 0-3 | signature | `50 4B 03 04` |
| 6-7 | general purpose flags | bits 0, 3, 6 and 13 clear (mask `0x2049`); bit 11 either way |
| 8-9 | compression method | `0` (STORE) |
| 18-21 | compressed size | index payload size: equal to the next field, above 0, below `0xFFFFFFFF` |
| 22-25 | uncompressed size | same value |
| 26-27 | name length | `9` |
| 28-29 | extra field length | `12` |
| 30-38 | name | `__cozip__` |
| 39-40 | extra header id | `0xCA0C` (bytes `0C CA`) |
| 41-42 | extra data size | `8` |
| 43-50 | integrity hash | u64, FNV-1a 64 (section 4) |

The index entry never uses ZIP64 and carries no other extra field, so the payload always
starts at byte 51 (`COZIP_INDEX_OFFSET`).

## 3. Index payload

| Section | Size | Content |
| --- | --- | --- |
| header | 11 | `CZIP`, u16 `version` = 1, u8 `profile`, u32 `n_entries` |
| name lengths | 2 x n | u16 per priority file, each above 0 |
| names | sum of lengths | ASCII, concatenated, no terminators |
| offsets | 8 x n | u64 payload offset from byte 0 (first byte after that entry's LFH) |
| sizes | 8 x n | u64 payload size, above 0 |

- Size: `11 + sum(18 + len(name))`. A Flat index (`__metadata__` only) is always 41 bytes.
- Only priority files are listed, never `__cozip__` or `__cozip_padding__`. Names are
  unique. Order carries no meaning unless a profile adds one.
- Readers reject a `version` above what they support. A reader that does not know the
  profile may still look up entries by literal name.
- `offset + size` must not overflow u64 or pass the end of the archive.

Reading the index needs two small range requests and no Central Directory. Verified
against a public fixture:

```python
import struct, urllib.request

def fetch(url, start, length):
    request = urllib.request.Request(
        url, headers={"Range": f"bytes={start}-{start + length - 1}", "User-Agent": "cozip"})
    with urllib.request.urlopen(request) as response:
        return response.read()

def read_index(url):
    head = fetch(url, 0, 51)
    index = fetch(url, 51, struct.unpack_from("<I", head, 18)[0])
    version, profile, n = struct.unpack_from("<HBI", index, 4)
    lengths = struct.unpack_from(f"<{n}H", index, 11)
    cursor, names = 11 + 2 * n, []
    for length in lengths:
        names.append(index[cursor:cursor + length].decode("ascii"))
        cursor += length
    offsets = struct.unpack_from(f"<{n}Q", index, cursor)
    sizes = struct.unpack_from(f"<{n}Q", index, cursor + 8 * n)
    return profile, dict(zip(names, zip(offsets, sizes)))

url = "https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures/resolve/v0.1.0/data/cities.zip"
read_index(url)   # (1, {'__metadata__': (2899, 11868)})
```

This skips the integrity check; use it only on trusted sources, or verify the hash too.

## 4. Integrity hash

- Input: the index region `[51, 51 + index_size)` followed by the suffix
  `[archive_size - 32768, archive_size)`, in ascending byte order, overlapping bytes once.
- Function: FNV-1a 64, offset basis `0xCBF29CE484222325`, prime `0x100000001B3`
  (`COZIP_FNV_OFFSET_BASIS`, `COZIP_FNV_PRIME`).
- The writer emits eight zero bytes at 43-50, finishes the whole ZIP, then patches the
  hash. That patch is the only mutation allowed, and it does not affect any ZIP CRC-32.
- Readers should verify it before trusting offsets from remote or cached copies
  (spec 8.5). The DuckDB extension and the JavaScript reader do.
- It detects accidental changes to the fast path: the index, the Central Directory, the
  end records and whatever else sits in the last 32 KiB. It is not a signature, and it does
  not cover payload bytes in the middle of a large archive. Appending a ZIP comment,
  rewriting the Central Directory or touching the index all break it.

## 5. Rules for every ZIP entry

- Compression method 0 (STORE), with equal compressed and uncompressed sizes above zero.
  An empty file cannot be an entry.
- No encryption: flag bits 0, 6 and 13 clear, method 99 unused, no central directory
  encryption.
- No data descriptors (flag bit 3 clear) and no explicit directory entries.
- Unique filenames made of bytes `0x01` to `0x7F`; flag bit 11 may be set or not.
- Single segment, no split or spanned archives, EOCD comment length 0.

## 6. Names

A name is non-empty, at most 65,535 bytes (the ZIP field width) and:

- has no leading `/`, no drive-letter prefix such as `C:`, and no trailing `/`;
- uses `/` as the only separator (the C writer also rejects `\`);
- has no `.` or `..` component;
- is not `__cozip__`, and is `__cozip_padding__` only for the padding entry.
  Profiles reserve more: Flat reserves `__metadata__`; TACO expects `COLLECTION.json` and
  `METADATA/*.parquet` as priority files.

Writers enforce this in `validate_archive_name_syntax` in `core/cozip.c`. Local source and
output paths are not archive names: they are UTF-8, and the Windows build converts them
to UTF-16.

## 7. Size floor and padding

- An archive must be at least 32,819 bytes (`COZIP_MIN_ARCHIVE_SIZE`, 32 KiB plus 51), so
  the hash bytes at 43-50 never fall inside the hashed suffix.
- The reference writer predicts the final size and, when it is short, inserts one entry
  named `__cozip_padding__` filled with `0x5A` (`Z`). The padded archive is exactly 32,819
  bytes, or slightly larger when the shortfall is below the 110 bytes of headers the
  padding entry itself adds.
- Flat and profile 0 append the padding after every other entry, so it follows
  `__metadata__`. TACO inserts it immediately before the final priority block.
- The padding entry is never indexed and never appears in a Flat manifest, but ZIP tools
  list it. Readers treat it as an ordinary entry without meaning.

## 8. ZIP64

- `__cozip__` is always ZIP32, which caps the index payload below 4 GiB.
- Other entries use ZIP64 when needed. A payload above `0xFFFFFFFF` bytes gets a 20-byte
  ZIP64 extra field in its LFH (id `0x0001`, both sizes), and its 32-bit size fields hold
  `0xFFFFFFFF`, so `payload_offset = lfh_offset + 30 + len(name) + 20`.
- A payload of exactly `0xFFFFFFFF` bytes is refused (`cannot be represented in a ZIP local
  header`): the vendored libzip writes an unreadable header at that one size.
- When offsets pass 4 GiB the archive also gets the ZIP64 end record and locator.
  Verified: a 4.29 GiB entry followed by a small file gave payload offsets 149 and
  4,294,979,829, and both the reader and `scripts/check_cozip.py` accepted the archive.

## 9. Profiles

| Value | Profile | Priority files | Reader location column |
| --- | --- | --- | --- |
| 0 | none | any; names documented by the producer | none defined |
| 1 | Flat | exactly `__metadata__`, a Parquet file | `cozip:location` |
| 2 | TACO | `COLLECTION.json` and every `METADATA/*.parquet` | `taco:location` |
| 3 to 255 | reserved | | |

Flat (spec 13):

- One manifest row per ZIP entry except `__cozip__`, `__metadata__` and
  `__cozip_padding__`.
- Required columns: `name` (string), `offset` (uint64), `size` (uint64). These names are
  reserved; any other column is allowed, including a GeoParquet geometry column with the
  `geo` file metadata (spec 13.6).
- `cozip:location` and `taco:location` must never be stored. Readers compute
  `/vsisubfile/{offset}_{size},A` for an archive opened at `A`, wrapping HTTP(S) URLs as
  `/vsicurl/A`, emit it by default, and discard a stored copy.
- Writers may place `__metadata__` last; the reference writer does (before padding).

TACO (spec 14): the priority files form one contiguous block right before the Central
Directory, padding may only precede that block, and a reader can fetch all of them with a
single range from the smallest priority offset to the largest end. See `taco-profile.md`.

## 10. Error codes

Spec 9 names the failures a reader should report:

| Code | Meaning |
| --- | --- |
| `INVALID_LFH` | the header at byte 0 fails a check from section 2 |
| `INVALID_MAGIC` | the index does not start with `CZIP` |
| `UNSUPPORTED_VERSION` | index `version` above what the reader supports |
| `UNKNOWN_PROFILE` | unsupported profile where the application needs profile semantics |
| `HASH_MISMATCH` | computed integrity hash differs from bytes 43-50 |
| `ARCHIVE_TOO_SMALL` | archive below 32,819 bytes |
| `TRUNCATED_INDEX` | index shorter than its declared entries and name lengths |
| `DUPLICATE_NAME` | a name repeats in the index or in the archive |
| `INVALID_NAME` | a name breaks section 6 |
| `MISSING_ENTRY` | an indexed name has no ZIP entry, or its offset or size disagree with it |
| `INVALID_OFFSET` | an offset and size overflow or pass the end of the archive |
| `INVALID_ZIP_STRUCTURE` | an entry breaks section 5 |
| `INVALID_ZIP64` | a ZIP64 sentinel without a valid ZIP64 value, or ZIP64 on the index entry |

These are reader codes. The C writer returns its own `cozip_status_t`
(`INVALID_ARGUMENT`, `IO`, `INVALID_LFH` for a failed post-write check, and so on; see
`c-api.md`). The JavaScript reader prefixes several messages with the spec code
(`cozip HASH_MISMATCH: ...`); the DuckDB extension uses plain sentences (`debugging.md`).

## 11. Validating an archive

`scripts/check_cozip.py` (standard library only) implements sections 2 to 7 and the Flat
priority rule for local files:

```bash
python scripts/check_cozip.py dataset.zip other.zip
```

```text
dataset.zip: ok, profile 1, 4 ZIP entries, priority files ['__metadata__']
other.zip: HASH_MISMATCH: stored 0x3d5fdd4ecb204934
```

It exits with status 1 when any archive fails. Crafted corruptions produced the expected
code for each case: flipped index or tail bytes, an appended comment and truncation give
`HASH_MISMATCH`; a wrong magic, version, entry count, offset or size with a repaired hash
give `INVALID_MAGIC`, `UNSUPPORTED_VERSION`, `TRUNCATED_INDEX`, `MISSING_ENTRY` and
`INVALID_OFFSET`; a plain ZIP gives `INVALID_LFH`.

It does not open the Parquet manifest. For Flat archives, add this check (needs
pyarrow); it accepts `large_string` names because geopandas writes them:

```python
import io
import pyarrow as pa
import pyarrow.parquet as pq
from check_cozip import check          # scripts/check_cozip.py on sys.path

RESERVED = {"__cozip__", "__metadata__", "__cozip_padding__"}

def check_flat_manifest(path):
    profile, priorities, entries = check(path)
    if profile != 1:
        raise ValueError(f"profile {profile} is not Flat")
    offset, size = priorities["__metadata__"]
    with open(path, "rb") as f:
        f.seek(offset)
        table = pq.read_table(io.BytesIO(f.read(size)))
    schema = table.schema
    if (schema.field("name").type not in (pa.string(), pa.large_string())
            or schema.field("offset").type != pa.uint64()
            or schema.field("size").type != pa.uint64()):
        raise ValueError("name must be a string column, offset and size uint64")
    if {"cozip:location", "taco:location"} & set(schema.names):
        raise ValueError("a reader-owned location column is stored")
    rows = table.select(["name", "offset", "size"]).to_pylist()
    manifest = {row["name"]: (row["offset"], row["size"]) for row in rows}
    data = {name: span for name, span in entries.items() if name not in RESERVED}
    if len(manifest) != len(rows) or manifest != data:
        raise ValueError("manifest rows do not match the data entries")
    return len(rows)
```

Both checks passed on archives from the Python, R, Julia and C writers and on the
GeoParquet fixtures; `check_cozip.py` also accepted the ZIP64 archive from section 8.

## 12. Worked offsets

A Flat archive with `a.txt` (96 bytes) and `data/b.bin` (40,960 bytes), written in that
order:

| Item | Offset |
| --- | --- |
| `__cozip__` LFH | 0 |
| index payload (41 bytes) | 51 |
| `a.txt` LFH | 92 = 51 + 41 |
| `a.txt` payload | 127 = 92 + 30 + 5 |
| `data/b.bin` LFH | 223 = 127 + 96 |
| `data/b.bin` payload | 263 = 223 + 30 + 10 |
| `__metadata__` LFH | 41,223 = 263 + 40,960 |

In general `lfh[i + 1] = payload[i] + size[i]` and
`payload[i] = lfh[i] + 30 + len(name[i]) (+ 20 for ZIP64 payloads)`. A name or size change
anywhere moves every later offset, which is why the manifest must be written after
planning and why the writer re-checks the plan against the file it produced.
