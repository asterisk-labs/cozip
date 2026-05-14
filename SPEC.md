# Cloud Optimized ZIP Format Specification

**Version** 1.0.1
**Binary format version** 1  
**Status** Stable  
**Date** 2026-05-09  
**License** CC BY 4.0

---

## Part I. Core format

## 1. Summary

A **cozip** file (Cloud Optimized ZIP) is a ZIP archive built for random access over byte-addressable storage. It targets workloads where a dataset contains many files and a consumer needs to reach any one of them quickly, in arbitrary order, without downloading the full archive. Machine learning training is the canonical example: a dataloader samples items in random order across an epoch, and every cold lookup must be fast.

A standard ZIP archive places its table of contents, the Central Directory, at the end of the file. A reader must first find and read that table before locating any entry. On archives with millions of files the Central Directory can be very large, and every current ZIP reader pays that cost. A cozip moves the fast access path to the start of the file.

Simply put, a compact binary index sits at byte 0 and lists the offsets and sizes of the files the creator chose to prioritize. A cozip-aware reader fetches that index and then jumps directly to any priority file. The Central Directory remains present and valid, so standard ZIP tools can still open the archive, but cozip-aware readers do not need the Central Directory for priority-file access.

In practice, priority files often include metadata containers such as Parquet, which encode structured, typed information about archive entries. Query engines such as DuckDB or Arrow can read this metadata to resolve byte ranges and access underlying data directly, treating the cozip like a database.

Part I defines the on-disk format and the semantics of the index. Part II defines optional **profiles** that fix the names and roles of priority files for specific use cases.

## 2. Conformance

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, **MAY**, and **OPTIONAL** in this specification are to be interpreted as described in RFC 2119 and RFC 8174.

Unless explicitly marked informative, all requirements in this document are normative.

## 3. Terminology

**Local File Header (LFH).** A ZIP record that precedes each entry's payload inside the archive.

**Central Directory (CD).** A table at the end of a ZIP archive listing all entries.

**End of Central Directory Record (EOCD).** The final ZIP record in a non-commented archive. It points to the Central Directory or, in ZIP64 archives, is paired with ZIP64 end records.

**Index entry.** The first ZIP entry in a cozip archive. Its filename is always `__cozip__`. Its payload holds the cozip index.

**Index.** The binary structure stored in the payload of the index entry, defined in section 7.

**Priority file.** A file the creator chose to list in the cozip index. Priority files are reachable by a cozip-aware reader without reading the Central Directory.

**Non-priority file.** A file present as a ZIP entry inside the archive but not listed in the cozip index. Non-priority files are reachable through the Central Directory or through profile metadata that names their byte ranges.

**Payload.** The raw bytes of a ZIP entry's content, immediately following that entry's Local File Header. Since cozip forbids compression, encryption, data descriptors, and explicit directory entries, the payload corresponds exactly to the original file bytes.

**Payload offset.** The byte position, measured from byte 0 of the archive, where a ZIP entry's payload begins.

**Half-open range.** An internal byte interval written `[start, end)`, including `start` and excluding `end`.

**Profile.** A named convention layered on top of the core format that fixes the names and roles of certain priority files.

## 4. Goals

1. Provide fast remote access to priority files inside a ZIP archive.
2. Remain a valid ZIP archive readable by standard ZIP tools.
3. Keep the format parseable in any language with minimal dependencies.
4. Prohibit ZIP features that break direct byte-range access: compression, encryption, data descriptors, split/spanned archives, explicit directory entries, and zero-byte entries.
5. Detect accidental structural modification of the cozip access path through a compact integrity hash.
6. Allow higher-level conventions through profiles without changing the core binary index.

## 5. Archive layout

A cozip archive is a valid, single-segment ZIP archive whose first entry is the index entry.

Byte 0 of the archive is the first byte of the Local File Header of the index entry. No header, padding, spanning marker, self-extracting executable, or metadata precedes it.

The top-level layout is:

```text
[LFH for __cozip__][cozip index payload] 
[file entry 1]
[file entry 2]
...
[file entry n]
[Central Directory]
[optional ZIP64 EOCD record and ZIP64 EOCD locator]
[EOCD]
```

The ZIP archive comment length in the EOCD **MUST** be zero. Therefore, the EOCD is the final ZIP record and the final bytes of the archive.

### 5.1 Core requirements

#### Index entry

1. The first four archive bytes **MUST** be the ZIP Local File Header signature `0x04034b50` in little-endian order.
2. The first ZIP entry **MUST** have filename `__cozip__`, encoded as the 9 ASCII bytes `5F 5F 63 6F 7A 69 70 5F 5F`.
3. The Local File Header of the index entry **MUST** carry exactly one extra field: the cozip integrity hash block defined in section 8 (12 bytes). Therefore the Local File Header has a fixed size of 51 bytes (30 + 9 + 12), and the cozip index payload always begins at archive byte 51.
4. The index entry **MUST NOT** use ZIP64. Its 32-bit `compressed_size` and `uncompressed_size` fields **MUST** be equal, greater than zero, and strictly less than `0xFFFFFFFF`.

#### All ZIP entries

5. Every ZIP entry **MUST** use compression method `0` (STORE), and its resolved compressed size and resolved uncompressed size **MUST** be equal and greater than zero. Explicit directory entries (zero-byte folder records) are not allowed.
6. Every ZIP entry **MUST** be unencrypted: General Purpose Bit Flag bits 0, 6, and 13 **MUST NOT** be set, compression method `99` **MUST NOT** be used, and archive-level Central Directory Encryption **MUST NOT** be used.
7. General Purpose Bit Flag bit 3 (Data Descriptor) **MUST NOT** be set for any entry.
8. All ZIP entry filenames **MUST** be valid UTF-8 path strings. General Purpose Bit Flag bit 11 (UTF-8 language encoding) **MUST** be set in both the Local File Header and the Central Directory Header for every entry whose filename contains any byte greater than or equal to `0x80`. For filenames composed entirely of bytes less than `0x80` (pure ASCII), bit 11 **MAY** be either set or unset, since ASCII is unambiguously valid UTF-8. This relaxation reflects the behavior of common ZIP writers (e.g. libzip).
9. All ZIP entry filenames **MUST** be unique within the archive.

#### Archive-level

10. The archive **MUST** be a single-segment ZIP file. Split, spanned, and multi-disk ZIP archives are not allowed.
11. The EOCD archive comment length **MUST** be zero.
12. The archive size **MUST** be at least `32768 + 51` bytes. This prevents the integrity-hash bytes (archive offsets 43 through 50) from falling inside the mandatory 32 KiB verification suffix.

### 5.2 ZIP64 policy

1. The `__cozip__` index entry **MUST NOT** use ZIP64 and **MUST NOT** contain a ZIP64 extra field.
2. Non-index entries and archive-level structures (Central Directory, EOCD) **MAY** use ZIP64 as defined in APPNOTE 6.3.10. When a 32-bit field carries the sentinel `0xFFFFFFFF`, or a 16-bit field carries `0xFFFF`, the corresponding ZIP64 extra field or ZIP64 end record **MUST** provide the real value. If ZIP64 is required for the archive, the writer **MUST** emit both the ZIP64 End of Central Directory Record and ZIP64 End of Central Directory Locator.

### 5.3 Name requirements

A cozip filename is a UTF-8 string stored in the ZIP filename field.

1. A filename **MUST NOT** be empty.
2. A filename **MUST NOT** start with `/`.
3. A filename **MUST NOT** contain a drive-letter prefix such as `C:`.
4. A filename **MUST** use `/` as the path separator when a path separator is needed.
5. A filename **MUST NOT** be `.` or `..`, and **MUST NOT** contain `/.` or `/..` path components.
6. The name `__cozip__` is reserved for the index entry and **MUST NOT** be used by any other entry. Profiles may reserve additional names.
7. The name `__cozip_padding__` is reserved. A writer **MAY** emit a single ZIP entry with this name to satisfy §5.1.12. Such an entry **MUST NOT** appear in the cozip index, and no other entry **MAY** use this name. Its payload contents and byte length are unspecified; readers **MUST** treat it as an ordinary non-priority ZIP entry with no semantic meaning.

## 6. Writer model

This section is normative for writers.

A cozip writer **MUST** construct a final, internally consistent ZIP archive. The recommended model is planned construction: before writing to the output object, the writer must compute all final archive metadata—Local File Header sizes, payload offsets and sizes, ZIP64 requirements, profile metadata, and Central Directory records.

The writer **MUST** know all final payload offsets and sizes before writing the index payload. The index payload, its CRC-32, and its 32-bit size fields **MUST** be final when the index entry is emitted.

The archive is finalized after the writer has:

1. generated all profile metadata, including Parquet metadata where required;
2. written the index entry with final index payload bytes;
3. written all non-index entries;
4. written the Central Directory and EOCD or ZIP64 EOCD structures;
5. computed and patched the cozip integrity hash (section 8.4).

The hash patch is the only mutation permitted during finalization. It does not invalidate any ZIP CRC-32, because ZIP CRC-32 covers entry payload bytes, not Local File Header extra-field metadata. After finalization the archive **MUST NOT** be modified.

*Informative.* A planned archive whose total predicted size is below the §5.1.12 minimum cannot be finalized as-is, because the integrity-hash bytes at offsets 43–50 would fall inside the 32 KiB suffix region and produce a self-referential hash input. To resolve this, the writer must extend the archive past the floor through any mechanism that does not violate the requirements of this section. The recommended mechanism, and the one used by the reference writer, is to append a single non-indexed entry named `__cozip_padding__` (§5.3.7), placed last so that user payload offsets remain stable across re-planning. The padding entry's payload contents and length are at the writer's discretion. Writers that satisfy §5.1.12 by other means (for example, by enlarging a profile-controlled metadata payload) are equally valid; the only spec-level requirement is that the resulting archive conform to Part I in full.

## 7. Index layout

The index is the payload of the `__cozip__` entry. It is organised as five contiguous sections.

```text
┌──────────────────────────────────────────────────────────────┐
│ Section 1. Header                         11 bytes           │
├──────────────────────────────────────────────────────────────┤
│ Section 2. Name lengths                   n_entries × 2      │
├──────────────────────────────────────────────────────────────┤
│ Section 3. Names (UTF-8 concatenated)     sum(name_lens) B   │
├──────────────────────────────────────────────────────────────┤
│ Section 4. Offsets                        n_entries × 8      │
├──────────────────────────────────────────────────────────────┤
│ Section 5. Sizes                          n_entries × 8      │
└──────────────────────────────────────────────────────────────┘
```

### 7.1 Header

| Offset | Size | Field       | Type   | Value                                   |
|-------:|-----:|-------------|--------|-----------------------------------------|
|      0 |    4 | `magic`     | bytes  | ASCII `CZIP` = `0x43 0x5A 0x49 0x50`    |
|      4 |    2 | `version`   | u16 LE | `1` for this binary format              |
|      6 |    1 | `profile`   | u8     | Profile identifier; see Part II         |
|      7 |    4 | `n_entries` | u32 LE | Number of priority files in the index   |

The header is 11 bytes long. A reader **MUST** reject the archive if `magic` is not `CZIP` or if `version` is greater than the reader supports.

A reader that does not recognise the declared `profile` **MAY** still parse the index and access priority files by literal name, but **MUST NOT** assume profile-specific semantics.

### 7.2 Name lengths

`n_entries` consecutive u16 little-endian values, giving the byte length of each name in section 7.3. Each name length **MUST** be greater than zero.

### 7.3 Names

The concatenation of all priority filenames, in the same order as section 7.2, encoded in UTF-8 without null terminators. Names within the index **MUST** be unique. The index **MUST NOT** include `__cozip__`.

### 7.4 Offsets

`n_entries` consecutive u64 little-endian values. Each value is the payload offset of the corresponding file, measured in bytes from byte 0 of the archive — that is, the first byte after that entry's Local File Header.

The order of entries in the index has no semantic meaning unless a profile adds a profile-specific ordering rule.

### 7.5 Sizes

`n_entries` consecutive u64 little-endian values. Each value is the byte length of the corresponding file's payload, and **MUST** be greater than zero.

For every indexed entry, the writer **MUST** ensure `offset + size` does not overflow u64 arithmetic and does not exceed the archive size. A reader **SHOULD** validate these conditions when the archive size is known.

## 8. Cozip integrity hash block

The cozip integrity hash block is a ZIP extra field attached to the Local File Header of the `__cozip__` entry. It carries an FNV-1a 64-bit hash over the cozip index payload and the final 32 KiB of the archive.

The hash is a compact structural integrity check designed to detect accidental modification of the fast access path: the index payload, the ZIP tail, the EOCD, and ZIP64 end structures. It is not a cryptographic signature and does not cover every data byte in the archive.

### 8.1 Block layout

The hash block is exactly 12 bytes long.

| LFH byte range | Block offset | Size | Field            | Type   | Value                                  |
|----------------|-------------:|-----:|------------------|--------|----------------------------------------|
| 39–40          |            0 |    2 | `header_id`      | u16 LE | `0xCA0C`                               |
| 41–42          |            2 |    2 | `data_size`      | u16 LE | `8`                                    |
| **43–50**      |            4 |    8 | `integrity_hash` | u64 LE | FNV-1a 64 over the hash input (§8.2)   |

Within the archive, the hash block occupies bytes 39 through 50 inclusive. The `integrity_hash` value occupies bytes 43 through 50; this is the **only** byte range mutated during finalization (§8.4).

### 8.2 Hash input

Let:

```text
INDEX_OFFSET      = 51
HASH_WINDOW_SIZE  = 32768
index_size        = resolved compressed size of the __cozip__ entry
index_region      = [INDEX_OFFSET, INDEX_OFFSET + index_size)
suffix_region     = [archive_size - HASH_WINDOW_SIZE, archive_size)
```

The archive size **MUST** be at least `HASH_WINDOW_SIZE + INDEX_OFFSET` (see 5.1.12).

The hash input is the union of `index_region` and `suffix_region`, hashed in ascending archive-byte order. If the two regions overlap, overlapping bytes are included only once.

Because the suffix region is 32 KiB and the ZIP archive comment is prohibited, the suffix region includes the EOCD and, when present, the ZIP64 EOCD Locator and ZIP64 EOCD Record.

### 8.3 Hash function

FNV-1a 64-bit is used because it is tiny to implement, fast, and sufficient for accidental corruption detection of a small structural window.

```text
offset_basis = 0xCBF29CE484222325
prime        = 0x100000001B3

h = offset_basis
for each byte b in input:
    h = h XOR b
    h = (h × prime) mod 2^64
return h
```

### 8.4 Writer procedure

A writer **MUST** initially write `integrity_hash` as eight zero bytes. After the full archive has been written, the writer **MUST** compute FNV-1a 64 over the hash input defined in §8.2 and overwrite the 8 bytes at archive offsets 43 through 50 with the computed value. No other bytes may be modified during finalization.

### 8.5 Reader verification

A reader **SHOULD** validate the integrity hash before trusting indexed offsets when the archive is read from remote storage or an untrusted cache.

The canonical validation procedure is:

1. Read the first 51 bytes of the archive and validate the index Local File Header:
   1. signature is `0x04034b50`;
   2. General Purpose Bit Flag bit 11 may be either set or unset; the literal `__cozip__` is pure ASCII, so the flag is informational for the index entry;   
   3. General Purpose Bit Flag bits 0, 3, 6, and 13 are **unset**;
   4. compression method is `0`;
   5. `compressed_size` and `uncompressed_size` are equal, greater than zero, and strictly less than `0xFFFFFFFF`;
   6. filename length is `9`;
   7. extra field length is `12`;
   8. filename equals the 9 ASCII bytes `5F 5F 63 6F 7A 69 70 5F 5F`;
   9. extra field `header_id` is `0xCA0C` and `data_size` is `8`.
2. Read `integrity_hash` from archive bytes 43–50.
3. Read the index payload `[51, 51 + index_size)`.
4. Obtain the archive size from `Content-Length`, `Content-Range`, filesystem metadata, or an equivalent byte-exact source.
5. Reject the archive if `archive_size < 32768 + 51`.
6. Read the suffix region `[archive_size - 32768, archive_size)`.
7. Compute FNV-1a 64 over the union of the index region and suffix region (§8.2).
8. Compare the computed value with the stored `integrity_hash`. If they differ, reject the archive with `HASH_MISMATCH`.

## 9. Error codes

Implementations **SHOULD** map parse and validation failures to the following canonical names.

| Code                          | Meaning |
|-------------------------------|---------|
| `INVALID_LFH`                 | The index Local File Header is malformed, not at byte 0, or fails any check in §8.5 step 1, including a non-conforming hash block. |
| `INVALID_MAGIC`               | Index header magic is not `CZIP`. |
| `UNSUPPORTED_VERSION`         | Index header version is greater than the reader supports. |
| `UNKNOWN_PROFILE`             | Index declares a profile the reader does not support and profile semantics are required by the application. |
| `HASH_MISMATCH`               | The computed integrity hash does not match the stored value. |
| `ARCHIVE_TOO_SMALL`           | The archive is smaller than `32768 + 51` bytes. |
| `TRUNCATED_INDEX`             | The index payload is shorter than required by `n_entries` and the declared name lengths. |
| `DUPLICATE_NAME`              | Two entries share the same name, either within the index or within the archive. |
| `INVALID_NAME`                | A filename violates §5.3 or is not valid UTF-8. |
| `MISSING_ENTRY`               | An indexed name has no corresponding ZIP entry, or its offset/size disagrees with the matching ZIP entry. |
| `INVALID_OFFSET`              | An offset/size pair overflows u64 arithmetic or extends beyond the archive size. |
| `INVALID_ZIP_STRUCTURE`       | A ZIP entry or archive-level structure violates §5.1: compression method other than STORE, encrypted, data descriptor present, explicit directory entry, zero or unequal sizes, UTF-8 flag inconsistent with §5.1.8, non-zero EOCD comment, or split/multi-disk archive. |
| `INVALID_ZIP64`               | A ZIP64 sentinel lacks a valid corresponding ZIP64 value, ZIP64 values are inconsistent, or the index entry uses ZIP64. |

---

## Part II. Profiles

## 12. Profile mechanism

A profile is a named convention layered on top of the core format. It fixes the names and roles of certain priority files so a reader that recognises the profile can rely on a known shape without inspecting producer-specific documentation.

A cozip declares its profile through the `profile` field of the index header (§7.1).

### 12.1 Profile values

| Value | Profile | Section |
|------:|---------|---------|
|     0 | None    | 12.2    |
|     1 | Flat    | 13      |
|     2 | TACO    | 14      |

Values from `3` to `255` are reserved for future use and **MUST NOT** be assigned by external specifications unless this document or a successor specification registers them.

### 12.2 Profile 0: None

A value of `0` means the cozip conforms to no profile. Priority filenames are free except for names reserved by the core format. Readers **MUST** inspect priority files by literal name as documented by the producer.

### 12.3 Profile constraints

A profile **MAY** reserve additional filenames, require certain priority files to exist, define their formats and schemas, fix their position in the archive, define conventions for non-priority files, and declare a profile-specific file extension or MIME type.

A profile **MUST NOT** modify or relax any requirement of Part I, including the binary structure of the cozip index and the Local File Header layout of the `__cozip__` entry. A registered profile value **MUST NOT** be reassigned or redefined by a successor profile.

### 12.4 Reader behavior

Reader behavior for unrecognized profiles is defined in §7.1. A reader **MAY** raise `UNKNOWN_PROFILE` when profile-specific guarantees are required by the application. A reader that supports the declared profile **SHOULD** validate the profile-specific requirements before exposing profile semantics to its caller.

## 13. Flat profile

The Flat profile describes a cozip whose priority metadata forms a single flat manifest for the archive's data entries.

### 13.1 Profile value

`profile = 1`.

### 13.2 Priority files

A Flat-profile cozip **MUST** contain exactly one priority file in its index, named `__metadata__`. The `__metadata__` file is a Parquet file.

Writers **MAY** place `__metadata__` as the last file entry in the archive before the Central Directory. This is a convenience for ZIP-only readers that prefetch the archive tail; cozip-aware readers locate it through the index regardless of position.

### 13.3 Schema

The `__metadata__` Parquet file **MUST** contain one row for every ZIP data entry except the `__cozip__` index entry, the `__metadata__` entry itself, and the `__cozip_padding__` entry if present (§5.3.7).

The Parquet file **MUST** contain at least the following columns:

| Column   | Type   | Meaning |
|----------|--------|---------|
| `name`   | string | The entry's filename in the archive. |
| `offset` | uint64 | The entry's payload offset, from byte 0 of the archive. |
| `size`   | uint64 | The entry's payload byte length. Greater than zero per §5.1.5. |

The `offset` and `size` values are computed by the writer at archive-creation time; the `name` value matches the filename of the corresponding ZIP entry. A producer **MAY** add additional columns to carry user-defined metadata. The names `name`, `offset`, and `size` are reserved by this profile.

### 13.4 File extension

A Flat-profile cozip uses the file extension `.zip`. The MIME type is `application/zip`.

## 14. TACO profile

The TACO profile describes a cozip whose priority files form the ZIP-mode access contract of a TACO dataset.

### 14.1 Profile value

`profile = 2`.

### 14.2 Priority files

A TACO-profile cozip **MUST** include the following priority files in its index:

1. `COLLECTION.json`;
2. every Parquet file under the `METADATA/` directory.

The `DATA/` and `METADATA/` layouts, and the contents of `COLLECTION.json` and the metadata Parquet files, are defined by the TACO specification (§14.4).

### 14.3 Position and contiguity

All TACO priority files **MUST** form one contiguous final file-entry block immediately before the Central Directory. No non-priority entry **MAY** appear between two TACO priority entries, and no entry **MAY** appear after the priority block. The relative order of priority files within this block is unspecified; readers locate each by name through the cozip index.

A `__cozip_padding__` entry (§5.3.7), when present, **MUST** be placed before the priority block; it **MUST NOT** appear inside or after it.

This guarantees that the priority payloads are addressable through one contiguous archive region. A reader **MAY** compute:

```text
priority_region_start = min(priority_payload_offsets)
priority_region_end   = max(priority_payload_offset + priority_payload_size)
```

and fetch:

```http
Range: bytes=<priority_region_start>-<priority_region_end - 1>
```

The returned region may contain Local File Headers between priority payloads; the reader slices out each payload using the offsets and sizes from the cozip index.

### 14.4 Reference semantics

The semantics of `COLLECTION.json`, the Parquet files under `METADATA/`, the `DATA/` layout, TACO metadata columns such as `internal:offset` and `internal:size`, and the dataset contract are defined by the TACO specification. This cozip profile only defines how the TACO ZIP-mode priority files are made byte-addressable through the cozip index.

### 14.5 File extension

A TACO-profile cozip uses the file extension `.zip`. The MIME type is `application/zip`.

---

## Appendices

## Appendix A. MIME type and file extension

A cozip archive uses the standard ZIP file extension `.zip` and MIME type `application/zip`. cozip is a structural profile of ZIP — analogous to how Cloud Optimized GeoTIFF uses `.tif` — and is detected by the presence of a `__cozip__` index entry at byte 0, not by extension. 

When serving `.zip` over HTTP, servers must preserve byte-exact object bytes for range requests. Transparent content encoding is incompatible with cozip byte offsets.

## Appendix B. Version history

| Version      | Date       | Changes |
|-------------:|------------|---------|
| 1.0-draft.1  | 2026-04-27 | Initial draft. |
| 1.0-draft.2  | 2026-04-27 | Added ZIP64 policy for non-index entries; explicitly prohibited encryption, split/spanned archives, data descriptors, directory entries, and zero-size entries; changed the integrity hash to cover the index payload plus the final 32 KiB; fixed HTTP byte-range inclusivity; made UTF-8 and filename uniqueness requirements explicit; fixed Flat profile `__metadata__` semantics; made TACO priority files contiguous; replaced pending TACO reference. |
| 1.0-draft.3  | 2026-04-28 | **Editorial refactor of Part I.** Removed the *Semantics and guarantees* section as fully redundant with §4–§8. Consolidated STORE+size, encryption, ZIP64-sentinel, name-uniqueness, archive-immutability, and hash-byte-range statements that were repeated across §5, §6, §7, §8, §9. Reorganised §5.1 into three groups (Index entry / All ZIP entries / Archive-level). Reduced §5.2 from six items to three. Made §8.5 step 1 the single canonical LFH validation list and added a missing check for GP bit 11 set and exact 9-byte filename equality; §10 step 2 now references it. Relaxed the §7.5 archive-size constraint from MUST to SHOULD on the reader side (writers still MUST). Reduced error codes from 24 to 15 by collapsing index/zip duplicate-name codes, name/UTF-8 codes, range/overflow codes, and per-entry violation codes into single canonical names. Fixed the APPNOTE URL in Appendix C. |
| 1.0-draft.4  | 2026-04-28 | **Editorial refactor of Part II.** Collapsed *what a profile MAY do* and *what a profile MUST NOT do* into a single profile-constraints section, and folded reader-behavior duplication into §7.1. Removed the Flat-profile §14.3 *position requirement* as a normative MUST; placement of `__metadata__` is now an informative writer hint. Removed the Flat-profile schema's *Must be unique* note for the `name` column (already covered archive-wide by §5.1.9) and clarified that an archive with no other data entries may carry a zero-row `__metadata__`. Removed from the TACO profile the prohibition on directory entries for `DATA/` and `METADATA/` (already prohibited archive-wide by §5.1.5). Clarified that the relative order of TACO priority files within the contiguous priority block is unspecified. Section numbering shifted: Profiles is now §12, Flat is §13, TACO is §14. |
| 1.0-draft.5  | 2026-04-29 | Relaxed §5.1.8 and §8.5 step 1.ii to require GP bit 11 (UTF-8) only when the filename contains bytes ≥ 0x80. Aligns the spec with common ZIP writer behavior and unblocks libzip builds that omit the flag for ASCII-only names. |
| 1.0          | 2026-05-03 | First stable release. Some redundant normative statements were removed during the draft phase, but no technical changes were made between 1.0-draft.5 and 1.0. |
| 1.0.1        | 2026-05-09 | Reserved the name `__cozip_padding__` in §5.3.7 as a writer-side mechanism for satisfying the §5.1.12 minimum archive size, and added an informative note in §6 documenting the recommended use. Excluded `__cozip_padding__` from the Flat-profile `__metadata__` row set (§13.3), and clarified its placement under the TACO profile (§14.3). No on-disk format change; archives produced under 1.0 remain valid under 1.0.1 without modification. |


## Appendix C. References

1. PKWARE Inc. **.ZIP File Format Specification**, APPNOTE.TXT, version 6.3.10, 2022. https://pkware.cachefly.net/webdocs/APPNOTE/APPNOTE-6.3.10.TXT
2. IETF. **HTTP Semantics**, RFC 9110, 2022. https://datatracker.ietf.org/doc/html/rfc9110
3. Bradner, S. **Key words for use in RFCs to Indicate Requirement Levels**, RFC 2119, 1997. https://datatracker.ietf.org/doc/html/rfc2119
4. Leiba, B. **Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words**, RFC 8174, 2017. https://datatracker.ietf.org/doc/html/rfc8174
5. Yergeau, F. **UTF-8, a transformation format of ISO 10646**, RFC 3629, 2003. https://datatracker.ietf.org/doc/html/rfc3629
6. Fowler, G., Noll, L. C., Vo, K. P. **FNV Hash**, non-cryptographic hash function. http://www.isthe.com/chongo/tech/comp/fnv/
7. Apache Software Foundation. **Apache Parquet Format**. https://parquet.apache.org/
8. Asterisk Labs. **The TACO Specification**, version 3.0.0, released 2026-04-13. https://asterisk.coop/taco/spec/