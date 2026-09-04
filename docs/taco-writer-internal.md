# TACO writer bridge

TACO uses libcozip to write the physical container. The native ABI is public;
`cozip._taco` is only the Python adapter and is not an application API.

## Where the boundary sits

TACO validates the contract, assigns archive names, and creates
`COLLECTION.json` and the `METADATA/*.parquet` files. It also owns staging,
overwrite policy, partitioning, and publication of the completed archive.

Libcozip handles the ZIP layout. It validates entry names, reads source sizes,
computes payload offsets, writes STORE entries, places the final priority
block, adds padding when needed, checks the headers it wrote, and patches the
cozip integrity field. Payloads are opaque bytes to this layer: libcozip does
not link Arrow, Parquet, or a JSON library.

That split keeps TACO changes out of the container ABI. An R or Julia TACO
writer can call the same two C functions through its own private adapter; it
does not need to route through Python.

## Two calls

```c
cozip_taco_plan_t plan = COZIP_TACO_PLAN_INIT;

cozip_status_t cozip_plan_taco(
    cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    cozip_taco_plan_t *out_plan,
    cozip_error_t *err);

cozip_status_t cozip_write_taco(
    const char *out_path,
    const cozip_path_entry_t *files,
    size_t n_files,
    const cozip_path_entry_t *priorities,
    size_t n_priorities,
    const cozip_taco_plan_t *plan,
    cozip_error_t *err);
```

`cozip_plan_taco` receives materialized data files and the ordered names of
the priority files. Priority payloads do not exist yet. Their names determine
the size of the byte-0 index, while their later sizes cannot move the data
entries that precede them. The call returns the data offsets that TACO writes
into its Parquet metadata.

TACO then materializes the priority files and calls `cozip_write_taco`.
Libcozip stats the sources again, rebuilds the layout, and rejects changes in
names, order, data sizes, or planned offsets before it opens the output.

`cozip_taco_plan_t.layout_hash` is a stale-plan check. It covers the profile,
entry roles, ordered names, counts, and data sizes. It does not cover file
contents and is not an authenticity or TOCTOU control. A source can still
change after either stat call, and a same-size change does not alter the
token. Callers that need a stable snapshot must stage immutable sources or
provide their own locking.

## Rules enforced by libcozip

- Archive names are portable ASCII names and unique across data and priority
  entries. `__cozip__` and `__cozip_padding__` are reserved.
- `COLLECTION.json` is a priority entry. Every
  `METADATA/<name>.parquet` entry is also in the priority block.
- Sources are non-empty regular files. Filesystem paths are UTF-8 and are not
  subject to the archive-name ASCII rule.
- The final priority block is contiguous, with optional padding immediately
  before it.
- An output that aliases one of its inputs is rejected.
- A payload of exactly `0xFFFFFFFF` bytes is rejected because the bundled
  libzip cannot encode that boundary correctly in a local header.

The set and contents of the metadata files remain TACO's responsibility.

## Python adapter

```python
from cozip._taco import plan, write

layout = plan(
    [("DATA/0/image.tif", "/tmp/image.tif")],
    ["COLLECTION.json", "METADATA/collection.parquet"],
)

# Build the JSON and Parquet files with layout.offsets, then write:
write(
    "/tmp/dataset.tmp",
    layout,
    [
        ("COLLECTION.json", "/tmp/COLLECTION.json"),
        ("METADATA/collection.parquet", "/tmp/collection.parquet"),
    ],
)
```

`Plan` is an in-process value, not a serialized format. The adapter initializes
and checks the native struct version before crossing the ABI.

## Publication and failures

The TACO writer gives libcozip a unique temporary path in the destination
directory. Only a complete, verified archive is published. With
`overwrite=False`, publication uses a no-replace operation so another writer
cannot be overwritten between the early existence check and publication.
With `overwrite=True`, the last successful publisher wins.

Two direct native calls must not write the same output path concurrently.
Libcozip may reopen that path while verifying and patching the hash; callers
must serialize access or, preferably, give each call a distinct temporary
path.

This is atomic publication, not crash durability. The writer does not call
`fsync` on the archive or its parent directory. A process failure can leave a
temporary file, and a machine failure immediately after publication may lose
the rename on filesystems that require explicit syncing.
