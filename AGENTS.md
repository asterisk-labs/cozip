# cozip repository guidance

cozip is a C11 library in `core/` that writes Cloud-Optimized ZIP archives and reads their
Flat manifest, with Python, R and Julia bindings over it, a pure JavaScript reader in
`javascript/`, and the format specification in `SPEC.md`. The reader fetches bytes through
karu, a C++23 library vendored in `core/karu/` that links libcurl and OpenSSL. The optional
DuckDB extension lives in its own repository, asterisk-labs/cozip_reader, and no cozip code,
test or install step loads it.

## Working agreements

- Read `SPEC.md` before changing behaviour and ground each rule in a spec section. Raise
  spec ambiguities instead of resolving them in code. A layout change starts in the spec,
  and published archives must stay readable.
- Edit only `core/cozip.c`, `core/cozip_read.c` and `core/cozip.h`. `make sync` copies
  them, the vendored libzip, zlib and karu, `VERSION` and `LICENSE` into `r/src/` and the
  packages; never edit `r/src/cozip.{c,h}`, `r/src/cozip_read.c`, `r/src/libzip/`,
  `r/src/zlib/`, `r/src/karu/`, `python/VERSION` or `python/LICENSE` by hand.
  `core/karu/` is a copy of a tagged karu release; update it whole, never patch it.
- Enforce physical rules shared by every writer (names, sizes, sources, layout, profile
  name rules) in the C core. Bindings check types, tables and Parquet contents, and keep
  Python, R and Julia behaviour and tests aligned.
- The C core treats payloads as opaque bytes and never parses Parquet or JSON. TACO
  semantics belong to the TACO package; `cozip._taco`, `cozip_plan_taco` and
  `cozip_write_taco` are its contract, documented in `docs/taco-writer-internal.md`.
- Readers compute `cozip:location` and `taco:location`; writers never store them. Archive
  names are ASCII, and every profile keeps the `.zip` extension.
- DuckDB extension changes go to cozip_reader. The bindings decode the fetched manifest
  with DuckDB itself, with extension autoloading and autoinstall turned off.
- Treat `core/build*/`, `python/cozip/_lib/`, `dist/`, `javascript/types/`, `r/man/`,
  `r/NAMESPACE`, `r/src/Makevars`, `r/src/Makevars.win` and `julia/Artifacts.toml` as
  generated. `index.html`, `playground.html` and `docs/*.html` are hand-maintained source.
- `VERSION` is the only version source.

## Validation

- There is no CI on pushes; run the relevant suites locally. C: configure `core` with
  `-DCOZIP_BUILD_TESTS=ON` and run `ctest`. The reader needs libcurl and OpenSSL, and
  `-DCOZIP_READER=OFF` builds the writer alone. Python: `make python`, or
  `python -m pytest python/tests` with `COZIP_LIB_PATH` pointing at a fresh build. R, Julia
  and JavaScript: `make r`, `make julia`, `make javascript`.
- Rebuild the library before testing a binding after C changes, and run `make sync` before
  committing them.
- For planning, layout, verification or hashing changes, also run the C and Python suites
  under ASan and UBSan and write a real archive above 4 GiB.

Detailed usage, format, binding, TACO, release and debugging procedures live in the
repository's `cozip` skill. Load the relevant reference from that skill instead of
expanding this always-on file with task-specific instructions.
