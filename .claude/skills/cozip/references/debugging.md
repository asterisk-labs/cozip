# Debugging

Messages below were captured from cozip 2026.9.27 with the native reader (Python, R,
Julia and C), the JavaScript reader and the DuckDB community extension 2.0.1 on DuckDB
1.5.5. libcozip messages, the reader's included, are the same in every binding; Python
shows them as `CozipError: [NAME] message`, R as `[cozip:NAME] message`, Julia as
`CozipError [NAME] message`. Match on the text.

## Contents

1. Writing
2. Staging a custom manifest
3. Reading a manifest
4. Library loading
5. Integrity hash mismatches
6. Wrong bytes, wrong offsets, missing columns
7. Silent outcomes worth checking
8. Reporting problems

## 1. Writing

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `[INVALID_ARGUMENT] entry 0 ('e') has a zero-byte payload; FLAT user entries must be non-empty` | empty file | drop it or give it content |
| `entry 0 archive name contains non-ASCII bytes; cozip archive names must be ASCII` | Unicode archive name | transliterate or percent-encode the name; local paths may stay Unicode |
| `archive name must be relative, not start with '/'` | absolute name | strip the leading `/` |
| `archive name must not have a drive-letter prefix` | `C:/...` | use a relative name |
| `archive name contains a '.' or '..' path component` | `a/../b`, `./x` | normalize the name |
| `archive name ends with '/'; explicit directory entries are not allowed` | directory entry | list files only |
| `archive name contains '\'; use '/' as the separator` | Windows separators | replace `\` with `/` |
| `archive name length must be 1 to 65535 bytes (got 0)` | empty or huge name | fix the name |
| `duplicate archive name 'x'` or binding `duplicate name 'x' at row 1` | repeated name | make names unique |
| `row 0 uses reserved name '__cozip_padding__'` | reserved name | rename |
| `output path is also the source path for entry 0 ('a')` | output would overwrite an input (same spelling or same file) | write elsewhere |
| `entry 0 ('a') source is 12 bytes, but the planned payload size is 10` | a source changed size between planning and writing inside one call (C callers: between `cozip_plan` and `cozip_write_archive`) | write from files nothing else modifies |
| `entry 0 payload size 0xFFFFFFFF cannot be represented in a ZIP local header` | a file of exactly 4,294,967,295 bytes, the one size the bundled libzip cannot write | store that file with any other size (split, pad or re-encode it) |
| `[IO] cannot stat 'path': No such file or directory` | missing source (C and TACO paths) | fix the path |
| `'path' is not a regular file` or binding `source is not a regular file` | directory, FIFO, device | pass files |
| `[IO] zip_close failed (Failure to create temporary file: No such file or directory)` | output directory missing or not writable | create it; check permissions and free space |
| `[INVALID_LFH] post-write check: ...` | the file libzip wrote differs from the plan; the output was removed | a bug: report it with the inputs (section 8) |
| `[IO] cleanup failed after ... invalid output may remain at '...'` | the failed output could not be deleted | delete it by hand; never publish it |
| `[IO] flushing hash to '...' failed` | disk full or I/O error at the end | free space and rerun |

Binding-side input errors (Python wording; R and Julia are equivalent):

| Message (excerpt) | Fix |
| --- | --- |
| `AttributeError: 'DataFrame' object has no attribute 'column_names'` | Python wants `pa.Table.from_pandas(df, preserve_index=False)` |
| `` cozip: `table` must be an arrow::Table (got data.frame) `` | R wants `arrow::as_arrow_table(df)` |
| `cozip: input table is missing required column(s): ['path']` | add `name` and `path` |
| `cozip: input table must not contain reserved column(s) ['offset']; the binding computes them` | drop `offset`, `size`, `cozip:location`, `taco:location` |
| `cozip: empty entry list` | at least one file |
| `cozip: row 0 name must be a string` | nulls or numbers in `name` |
| `cozip: row 0 ('a'): source not found: /path` | fix the path |

## 2. Staging a custom manifest

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `cozip: name mismatch at row 0: parquet='a', paths='b'` | Parquet rows and `paths` are in different orders | apply the same sort to both, or stage again |
| `cozip: offset mismatch at row 0 ('a'): parquet=0, plan=127` | offsets edited, or computed for another order or names | write the Parquet from `stage_metadata` output |
| `cozip: size mismatch at row 0 ('a'): parquet=10, plan=12` (or an offset mismatch on a later row) | a source changed after `stage_metadata` | stage again; with `validate=False` this goes unnoticed and the manifest is wrong |
| `cozip: metadata parquet has 1 rows, paths has 2` | filtered one side only | filter before `stage_metadata` |
| `cozip: metadata parquet has NULL name at row 1` | nulls in required columns | fill or drop rows before staging |
| `cozip: metadata parquet must not contain a 'path' column. ...` | `path` written into the manifest | drop it |
| `cozip: metadata parquet must not contain reader-owned column(s): ['cozip:location']` | a location column was saved | drop it; readers compute it |
| `cozip: metadata parquet is missing required column(s): ['size']` | column renamed or dropped | keep `name`, `offset`, `size` |
| `cozip: metadata parquet is empty` / `metadata parquet not found` | wrong file | point at the written Parquet |
| `cozip: paths must be a sequence of (name, source_path) pairs` | a string or table passed as `paths` | pass the list returned by `stage_metadata` |
| `cozip: validate must be a boolean` | `validate="yes"` | `True` or `False` |

## 3. Reading a manifest

Python, R and Julia (`read()`):

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `cozip.read: source contains non-ASCII characters; cozip supports ASCII paths and URLs only` | Unicode path or URL | rename, or percent-encode the URL |
| `cozip.read: columns must be a sequence of non-empty strings` | `columns="split"` | `columns=["split"]` |
| `[PROFILE] read_flat needs a Flat-profile archive (profile=1). Got profile=2 in: ...` | TACO archive (`profile=0` for profile 0) | read TACO with the TACO package |
| `[INVALID_LFH] LFH does not match cozip layout: ...` or `[INVALID_LFH] byte 0 is not a ZIP Local File Header: ...` | a plain ZIP, a bare Parquet file or another file | not a cozip |
| `[ARCHIVE_TOO_SMALL] cozip archive too small (minimum is 32819 bytes): ...` | wrong object, an HTML error page, a truncated upload | check the URL and object size |
| `[INVALID_INDEX] cozip integrity hash mismatch: ...` | modified or re-encoded bytes | section 5 |
| `[INVALID_INDEX] cozip index has no entry named '__metadata__': ...` | profile byte 1 without a Flat index | rebuild the archive |
| `[IO] /path/missing.zip: No such file or directory` | wrong local path | relative paths resolve against the process working directory |
| `[NOT_FOUND] https://host/missing.zip: HTTP 404: ...` | wrong URL or revision | fix the URL |
| a message with `HTTP 401` or `HTTP 403` | private object without credentials, expired presigned URL | set the credentials (`HF_TOKEN`, `AWS_*` and so on, `python-api.md` section 8), or use a fresh URL |
| `[INVALID_ARGUMENT] unknown scheme in 's4://...'` | a scheme karu does not read | use a local path or a supported URL (`python-api.md` section 5) |
| `[INVALID_ARGUMENT] could not read hf://datasets/a: expected hf://owner/name/path in 'a'` | incomplete `hf://` URL | add the owner, repository and file path |
| `Binder Error: Referenced column "x" not found` (a DuckDB exception) | a `columns` entry the manifest does not have | check the manifest's columns |
| `[UNSUPPORTED] this libcozip was built without the Flat reader` | library built with `-DCOZIP_READER=OFF`, or an R build for webR | rebuild with the reader |
| `Cozip.read is not supported on Windows yet` (Julia) | the native reader is not verified on Windows from Julia | read from Python or R on Windows |

JavaScript messages are listed in `javascript-api.md` section 7; SQL-only messages in
`duckdb-reader.md` section 8.

## 4. Library and extension loading

| Symptom | Meaning | Fix |
| --- | --- | --- |
| `ImportError: cozip: native library 'cozip.dylib' not found at .../cozip/_lib/cozip.dylib.` | source checkout without a staged library | `make lib`, or `COZIP_LIB_PATH=/abs/path/cozip.dylib` |
| `ImportError: cozip: COZIP_LIB_PATH='/nope' does not exist` | override points nowhere | fix the path |
| Julia `cozip: COZIP_LIB_PATH=... does not exist` or `cozip: native library not found at ...` | override or artifact missing | set the variable before `using Cozip`, or reinstall to fetch the artifact |
| C changes not visible in Python, R or Julia | the binding loads an older library | `make lib` (Python, Julia with `COZIP_LIB_PATH`), reinstall the R package |
| R source install fails compiling `libzip/*.c` | no C toolchain or GNU make (Rtools on Windows) | install the toolchain; no system libzip is needed |
| R `configure: pkg-config is required to find libcurl and OpenSSL` or `configure: libcurl >= 7.83.0 and OpenSSL >= 3.0 are required` | the reader's system libraries are missing | install pkg-config, libcurl and OpenSSL development files (`libcurl4-openssl-dev`, `libssl-dev` on Debian and Ubuntu) |
| R `C++23 standard requested but CXX23 is not defined` | R older than 4.3 or a compiler without C++23 | upgrade R or the toolchain |
| CMake `Could NOT find CURL` or `Could NOT find OpenSSL` in `make lib` or an sdist build | the reader's system libraries are missing | install them, or configure `-DCOZIP_READER=OFF` for a writer-only library |

`read()` never installs or loads a DuckDB extension, so a missing `cozip` or `httpfs`
extension, an old extension build or no network for `INSTALL` no longer affect it.
`COZIP_EXTENSION` is ignored. For the optional extension itself see `duckdb-reader.md`
section 1.

## 5. Integrity hash mismatches

The hash covers the index and the last 32 KiB. A mismatch means those bytes are not the
ones the writer produced:

1. The archive was modified after writing: adding a file or a comment with `zip` (both
   verified to break the hash), re-zipping, an archive manager that rewrote the Central
   Directory, or a second write under the same name while someone read it.
2. The bytes were transformed in transit: a server or CDN applying `Content-Encoding`, a
   proxy that rewrites responses, a text-mode transfer.
3. The copy is truncated or mixes versions: an interrupted upload, or a cache serving a
   different object for some ranges.

Diagnose on a local copy: `python scripts/check_cozip.py archive.zip`, compare its size and
SHA-256 with the file that was written, and compare `Content-Length` with the local size.
Rebuild with cozip rather than patching; the hash cannot be repaired in place without
rewriting it as the writer does.

## 6. Wrong bytes, wrong offsets, missing columns

- Bytes do not match the source file: check the HTTP range end (`offset + size - 1`,
  inclusive), then run `check_flat_manifest` (`format.md` section 11), which catches a
  manifest whose rows disagree with the ZIP entries (for example after
  `stage_create(validate=False)` with an edited Parquet).
- JavaScript `TypeError: Cannot mix BigInt and other types`: offsets and sizes are
  `BigInt` (`javascript-api.md` section 5).
- `cozip:location` missing after a projected read: pass `location=True`; the required
  columns and the location always come first in a projection.
- GDAL cannot open a location: a relative archive path used from another directory, a
  `/vsis3/`, `/vsigs/` or `/vsiaz/` location without GDAL credentials, a private
  `hf://` location without GDAL HTTP credentials, or a location
  made by DuckDB extension 2.0.x from an `hf://` revision (`duckdb-reader.md` section 6).
- An `__index_level_0__` column in the manifest: the table came from pandas without
  `preserve_index=False`.

## 7. Silent outcomes worth checking

- `zipfile.ZipFile(...).namelist()` shows `__cozip__`, `__metadata__` and possibly
  `__cozip_padding__`: expected; filter them when listing data files.
- R under a non-UTF-8 locale writes names such as `ni<c3><b1>o` instead of rejecting
  `niño` (`r-api.md` section 7).
- `stage_create` accepts `offset` and `size` stored as int64; the spec asks for uint64.
- Generic `cozip_finalize` can write a nonconforming Flat index; the Flat reader
  rejects it.
- In SQL with extension 2.0.1, a location column stored in the Parquet passes through
  `read_flat`; `read()` in Python, R and Julia drops stored `cozip:location`,
  `cozip:gdal_vsi` and `taco:location` columns and computes the location again.

## 8. Reporting problems

- Keep the failing inputs: the table or `paths` list, the manifest Parquet, file sizes, and
  the archive when one was produced. `check_cozip.py` output and the exact message help.
- Post-write check failures (`INVALID_LFH` from a writer) point at libcozip or libzip, not at
  user input.
- Reader crashes or hangs on malformed archives belong to the reader's repository:
  asterisk-labs/cozip for the native reader, the JavaScript reader and the bindings,
  asterisk-labs/cozip_reader for the DuckDB extension.
