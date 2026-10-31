# Changelog

Notable user-visible changes are recorded here.

## [Unreleased]

## [0.4.0] - 2026-09-27

### Added

- `karu_client_read_ends` / `Client::read_ends`: the first and last bytes of an object
  with its size and ETag in one round trip. The tail uses a suffix range, except on
  Azure, where it follows the head.

### Fixed

- Anonymous and SAS Azure requests send `x-ms-version: 2023-11-03`, like signed ones.
  Azure served anonymous reads as version 2009-09-19, whose unquoted ETags
  `karu_client_stat` dropped, so later reads could not be pinned with `if_match`. A SAS
  still authorizes with its own `sv`.

## [0.3.0] - 2026-09-26

### Added

- `karu_client_stat` / `Client::stat`: size and strong ETag in one probe.
- `karu_batch_get_stats` / `Batch::stats`: transfer, byte, retry and connection counters.
- Interrupted reads resume when a strong ETag pins the object version; otherwise
  they restart the range.

### Changed

- Clients now use 4 I/O threads. `KARU_IO_THREADS` accepts 1–64, capped by
  `KARU_CONCURRENCY`; set it to 1 for the previous thread count.
- `KARU_MAX_ATTEMPTS` defaults to 8 instead of 3. DNS and connection failures
  still stop after 3 attempts. All attempts share the request deadline.
- New connections resolve and shuffle addresses, adding DNS lookups.
- Reduced repeated signing, configuration parsing and worker wakeups. Receive
  buffers use 256 KiB on libcurl 8.7+ and 64 KiB on older versions.

### Fixed

- GCS range reads preserve stored gzip bytes and reject reported transformations.
- Reads reject a response ETag that contradicts a single strong `if_match`.
- Resumed reads handle servers that ignore `Range` without inflating the fallback limit.
- Response header comparisons no longer depend on the process locale.
- Inherited batches return `KARU_ERR_INVALID` after `fork()`; freeing them cannot
  wait for the parent's workers.

## [0.2.3] - 2026-09-15

### Fixed

- POSIX credential helpers use `posix_spawn` instead of running non-async-signal-safe
  setup after `fork`. Process-group timeouts and output limits remain unchanged.

## [0.2.2] - 2026-09-12

### Added

- A new "How Karu Reads" guide explains request planning, range coalescing,
  retries and credential resolution from end to end.

### Fixed

- Requests configured with `KARU_HTTP_HEADERS` reject cross-origin redirects
  before custom headers can be forwarded to another server. Same-origin
  redirects and headerless cross-origin redirects remain supported.
- Credential lookups keep the completed single-flight result alive while every
  caller copies it, closing a remaining race detected by ThreadSanitizer.

## [0.2.1] - 2026-09-10

### Fixed

- macOS discovers the host system CA bundle at runtime instead of retaining a
  certificate path from the build environment.

## [0.2.0] - 2026-09-10

### Added

- An optional planner benchmark records transfer counts, byte amplification and
  planning cost for contiguous and sparse range batches.
- C and C++ callers can override coalescing for one submitted batch without
  rebuilding the client.
- Karu sends `karu/<version>` as its default User-Agent. Set
  `KARU_HTTP_USER_AGENT` to replace it or to an empty value to omit it.
- CI measures line coverage of the C++ core and checks exact AWS SigV4, GCS
  GOOG1 and Azure SharedKey signatures against fixed vectors.
- The loopback integration suite covers range reads, size probes,
  preconditions, error mapping and credential refresh for every backend.

### Changed

- The C and C++ APIs now define which bytes may be inspected after a failed or
  short read.
- Merged HTTP ranges are scattered directly into their destination buffers;
  gaps no longer allocate staging memory or add a second copy.
- Local batches only merge ranges that overlap or touch.
- HTTP and GCS configuration use Karu-owned names. `KARU_HTTP_*` replaces the
  old transport namespace, while GCS options consistently begin with `GCS_`.
- URI paths, chaining and the C API remain compatible with 0.1.
- Credential discovery and refresh run outside the network scheduler.
  Independent paths may resolve concurrently; simultaneous requests for one
  scope share a single refresh.
- Temporary credentials refresh during the last ten percent of their lifetime,
  within a 1–60 second window. An authentication rejection invalidates the
  cached value and retries once with fresh credentials.
- Range reads and size probes now share response-state reset, `Retry-After`
  parsing and backoff behavior.

### Fixed

- Path-specific options and credential scopes stop at path boundaries, so a
  rule for `s3://data` cannot apply to `s3://data-private`.
- AWS and Source `credential_process` helpers are terminated at
  `KARU_REQUEST_TIMEOUT` and cannot return unbounded output.
- Concurrent credential lookups keep their shared refresh state alive until
  every waiting caller has resumed.
- Expired credentials are rejected instead of entering the cache.
- HTTP-date values in `Retry-After` are honored by size probes.

## [0.1.1] - 2026-09-09

### Fixed

- Linux discovers the host system CA bundle at runtime instead of retaining a
  certificate path from the build environment.

## [0.1.0] - 2026-09-09

First public release.

[Unreleased]: https://github.com/asterisk-labs/karu/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/asterisk-labs/karu/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/asterisk-labs/karu/compare/v0.2.3...v0.3.0
[0.2.3]: https://github.com/asterisk-labs/karu/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/asterisk-labs/karu/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/asterisk-labs/karu/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/asterisk-labs/karu/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/asterisk-labs/karu/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/asterisk-labs/karu/releases/tag/v0.1.0
