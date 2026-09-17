# cozip repository guidance

cozip is a C11 library in `core/` that writes Cloud-Optimized ZIP archives, with Python, R
and Julia bindings over it, a pure JavaScript reader in `javascript/`, and the format
specification in `SPEC.md`. The DuckDB reader lives in the `duckdb/` submodule
(asterisk-labs/cozip_reader).

## Working agreements

- Read `SPEC.md` before changing behaviour and ground each rule in a spec section. Raise
  spec ambiguities instead of resolving them in code. A layout change starts in the spec,
  and published archives must stay readable.
- Edit only `core/cozip.c` and `core/cozip.h`. `make sync` copies them, the vendored
  libzip and zlib, `VERSION` and `LICENSE` into `r/src/` and the packages; never edit
  `r/src/cozip.{c,h}`, `r/src/libzip/`, `r/src/zlib/`, `python/VERSION` or
  `python/LICENSE` by hand.
- Enforce physical rules shared by every writer (names, sizes, sources, layout, profile
  name rules) in the C core. Bindings check types, tables and Parquet contents, and keep
  Python, R and Julia behaviour and tests aligned.
- The C core treats payloads as opaque bytes and never parses Parquet or JSON. TACO
  semantics belong to the TACO package; `cozip._taco`, `cozip_plan_taco` and
  `cozip_write_taco` are its contract, documented in `docs/taco-writer-internal.md`.
- Readers compute `cozip:location` and `taco:location`; writers never store them. Archive
  names are ASCII, and every profile keeps the `.zip` extension.
- Treat `duckdb/` as an upstream submodule: DuckDB reader changes go to cozip_reader.
  Treat `core/build*/`, `python/cozip/_lib/`, `dist/`, `javascript/types/`, `r/man/`,
  `r/NAMESPACE` and `julia/Artifacts.toml` as generated. `index.html`, `playground.html`
  and `docs/*.html` are hand-maintained source.
- `VERSION` is the only version source.

## Validation

- There is no CI on pushes; run the relevant suites locally. C: configure `core` with
  `-DCOZIP_BUILD_TESTS=ON` and run `ctest`. Python: `make python`, or
  `python -m pytest python/tests` with `COZIP_LIB_PATH` pointing at a fresh build. R, Julia
  and JavaScript: `make r`, `make julia`, `make javascript`.
- Rebuild the library before testing a binding after C changes, and run `make sync` before
  committing them.
- For planning, layout, verification or hashing changes, also run the C and Python suites
  under ASan and UBSan and write a real archive above 4 GiB.

Detailed usage, format, binding, TACO, release and debugging procedures live in the
repository's `cozip` skill. Load the relevant reference from that skill instead of
expanding this always-on file with task-specific instructions.
