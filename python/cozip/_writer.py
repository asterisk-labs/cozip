"""High-level archive writer (FLAT profile only, path sources only).

Three operations layered top to bottom:

    create(out_path, table)                                all-in-one (most common)
    stage_metadata(table)                                  pure: plan offsets and sizes
    stage_create(out_path, paths, metadata_parquet)        pack from user-written parquet
"""

import os
import tempfile
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq
import pyarrow.compute as pc


from ._core import (
    COZIP_SOURCE_PATH,
    CozipError,
    ffi,
    lib,
)


# Reserved entry names; source of truth is libcozip.
_INDEX_NAME = ffi.string(lib.cozip_index_name()).decode("utf-8")
_PADDING_NAME = ffi.string(lib.cozip_padding_name()).decode("utf-8")
_METADATA_NAME = ffi.string(lib.cozip_flat_metadata_name()).decode("utf-8")
_RESERVED_NAMES = frozenset({_INDEX_NAME, _METADATA_NAME, _PADDING_NAME})

# Columns the binding computes; rejected if present in the input table.
_RESERVED_INPUT_COLUMNS = frozenset({"offset", "size"})

# Columns required in the metadata parquet that goes into the archive.
_REQUIRED_METADATA_COLUMNS = frozenset({"name", "offset", "size"})


def _check(status: int, err: Any) -> None:
    if status != 0:
        raise CozipError.from_struct(err)


def _validate_input_table(table: pa.Table) -> None:
    """Validate a (name, path, ...) input table for stage_metadata / create."""
    cols = set(table.column_names)
    missing = {"name", "path"} - cols
    if missing:
        raise ValueError(
            f"cozip: input table is missing required column(s): {sorted(missing)}"
        )
    reserved_in_input = sorted(_RESERVED_INPUT_COLUMNS & cols)
    if reserved_in_input:
        raise ValueError(
            f"cozip: input table must not contain reserved column(s) "
            f"{reserved_in_input}; the binding computes them"
        )
    if len(table) == 0:
        raise ValueError("cozip: empty entry list")

    names = table.column("name").to_pylist()
    paths = table.column("path").to_pylist()

    seen: set[str] = set()
    for i, name in enumerate(names):
        if name in _RESERVED_NAMES:
            raise ValueError(f"cozip: row {i} uses reserved name {name!r}")
        if name in seen:
            raise ValueError(f"cozip: duplicate name {name!r} at row {i}")
        seen.add(name)

    for i, p in enumerate(paths):
        if not Path(p).exists():
            raise FileNotFoundError(
                f"cozip: row {i} ({names[i]!r}): source not found: {p}"
            )


def _validate_paths_arg(paths: list[tuple[str, str]]) -> tuple[list[str], list[str]]:
    """Validate the (name, path) list passed to stage_create."""
    if len(paths) == 0:
        raise ValueError("cozip: empty paths list")

    names = [n for n, _ in paths]
    src_paths = [p for _, p in paths]

    seen: set[str] = set()
    for i, name in enumerate(names):
        if name in _RESERVED_NAMES:
            raise ValueError(f"cozip: paths[{i}] uses reserved name {name!r}")
        if name in seen:
            raise ValueError(f"cozip: duplicate name {name!r} at paths[{i}]")
        seen.add(name)

    for i, p in enumerate(src_paths):
        if not Path(p).exists():
            raise FileNotFoundError(
                f"cozip: paths[{i}] ({names[i]!r}): source not found: {p}"
            )

    return names, src_paths


def _alloc_entries(
    names: list[str],
    paths: list[str],
    n_extra: int,
) -> tuple[Any, list[Any]]:
    """Allocate cozip_entry_t[len(names) + n_extra] with user slots filled

    Returns (entries, keepalive). `keepalive` owns every cdata that
    libcozip reads through pointers in `entries`. Caller MUST keep it
    alive across every C call.
    """
    n = len(names)
    entries = ffi.new(f"cozip_entry_t[{n + n_extra}]")
    keepalive: list[Any] = []
    for i in range(n):
        name_c = ffi.new("char[]", names[i].encode("utf-8"))
        path_c = ffi.new("char[]", paths[i].encode("utf-8"))
        keepalive.extend((name_c, path_c))
        entries[i].arc_name      = name_c
        entries[i].payload_size  = Path(paths[i]).stat().st_size
        entries[i].in_index      = False
        entries[i].source.kind   = COZIP_SOURCE_PATH
        entries[i].source.u.path = path_c
    return entries, keepalive


def _temp_parquet_path(temp_dir: str | Path | None) -> Path:
    if temp_dir is not None:
        Path(temp_dir).mkdir(parents=True, exist_ok=True)
    fd, path = tempfile.mkstemp(
        suffix=".parquet",
        dir=str(temp_dir) if temp_dir is not None else None,
    )
    os.close(fd)
    return Path(path)


def _check_parquet_schema(parquet_path: str) -> None:
    """Structural check of the metadata parquet. Always run, regardless
    of `validate`, because it enforces the contract that `path` must
    not be in `__metadata__`.
    """
    schema = pq.read_schema(parquet_path)
    cols = set(schema.names)

    # paranoia? maybe 
    if "path" in cols:
        raise ValueError(
            "cozip: metadata parquet must NOT contain a 'path' column. "
            "`path` is filesystem-local and does not belong inside the "
            "archive. Drop it before writing the parquet."
        )

    missing = _REQUIRED_METADATA_COLUMNS - cols
    if missing:
        raise ValueError(
            f"cozip: metadata parquet is missing required column(s): "
            f"{sorted(missing)}"
        )


def _validate_parquet_values(
    parquet_path: str,
    expected_names: list[str],
    expected_offsets: list[int],
    expected_sizes: list[int],
) -> None:
    """Value-level check, fully vectorized via Arrow compute.

    Compares the parquet contents against the plan computed from the
    user-supplied paths. Every comparison happens inside Arrow C++:
    no Python iteration, no NumPy.
    """
    table = pq.read_table(parquet_path, columns=["name", "offset", "size"])

    n = len(table)
    if n != len(expected_names):
        raise ValueError(
            f"cozip: metadata parquet has {n} rows, "
            f"paths has {len(expected_names)}"
        )

    # Build the "expected" side once, in C, from the Python lists.
    exp_names = pa.array(expected_names, type=pa.string())
    exp_off   = pa.array(expected_offsets, type=pa.uint64())
    exp_sz    = pa.array(expected_sizes,   type=pa.uint64())

    # Cast the parquet integer columns to uint64.
    pq_names = table.column("name")
    pq_off   = pc.cast(table.column("offset"), pa.uint64())
    pq_sz    = pc.cast(table.column("size"),   pa.uint64())

    def _check(field: str, pq_col, exp_col, exp_list) -> None:
        eq = pc.equal(pq_col, exp_col)
        if pc.all(eq).as_py():
            return
        i = pc.index(eq, False).as_py()
        pq_val = pq_col[i].as_py()
        exp_val = exp_list[i]
        if field == "name":
            raise ValueError(
                f"cozip: name mismatch at row {i}: "
                f"parquet={pq_val!r}, paths={exp_val!r}"
            )
        raise ValueError(
            f"cozip: {field} mismatch at row {i} ({expected_names[i]!r}): "
            f"parquet={pq_val}, plan={exp_val}"
        )

    _check("name",   pq_names, exp_names, expected_names)
    _check("offset", pq_off,   exp_off,   expected_offsets)
    _check("size",   pq_sz,    exp_sz,    expected_sizes)

   
# Public API

def stage_metadata(
    table: pa.Table,
) -> tuple[pa.Table, list[tuple[str, str]]]:
    """Compute offsets and sizes for a cozip archive.
    
    The user is free to:
      * add extra columns to the metadata table (geometry, bbox,
        custom attributes);
      * write the parquet with any tool and options (pyarrow,
        DuckDB with spatial loaded, geopandas, ...);
      * pass the parquet plus the returned `paths` to `stage_create`.

    INVARIANT: the returned table and `paths` are aligned positionally.
    If the metadata table is reordered (e.g. through a DuckDB sort)
    before being written, `paths` MUST be reordered the same way, or
    `stage_create(validate=True)` will reject the mismatch.

    Args:
        table: pyarrow.Table with at least `name` (str) and `path`
            (str) columns. Extras are preserved in the returned table.
            Reserved columns `offset` or `size` in the input are
            rejected.

    Returns:
        Tuple of:
          * pyarrow.Table with `name`, `offset`, `size`, then any user
            extras. `path` is dropped because filesystem paths do not
            belong inside the archive.
          * list of (name, path) tuples, aligned with the table rows,
            ready to pass to `stage_create`.
    """
    _validate_input_table(table)

    n_users = len(table)
    names = table.column("name").to_pylist()
    src_paths = table.column("path").to_pylist()

    entries, _keepalive = _alloc_entries(names, src_paths, n_extra=1)
    err = ffi.new("cozip_error_t*")
    _check(lib.cozip_plan_flat(entries, n_users, err), err)

    offsets = [int(entries[i].payload_offset) for i in range(n_users)]
    sizes = [int(entries[i].payload_size) for i in range(n_users)]

    out = (
        table
        .drop_columns(["path"])
        .append_column("offset", pa.array(offsets, type=pa.uint64()))
        .append_column("size", pa.array(sizes, type=pa.uint64()))
    )
    rest = [c for c in out.column_names if c not in ("name", "offset", "size")]
    metadata_table = out.select(["name", "offset", "size", *rest])

    paths = list(zip(names, src_paths))
    return metadata_table, paths


def stage_create(
    out_path: str | Path,
    paths: list[tuple[str, str]],
    metadata_parquet: str | Path,
    validate: bool = True,
) -> str:
    """Pack a cozip archive from source files and a user-written parquet.

    The metadata parquet is embedded VERBATIM as the `__metadata__`
    entry inside the archive. cozip does not read, modify, or rewrite
    it. Whatever schema metadata, encoding, compression, and extra
    columns the user wrote are preserved bit-perfect.

    Args:
        out_path: destination cozip archive.
        paths: list of (name, source_path) tuples. Order MUST match the
            row order of `metadata_parquet`. `name` is what appears in
            the archive; `source_path` is where to read bytes from.
        metadata_parquet: path to the user-written parquet that becomes
            the `__metadata__` entry inside the archive. Must contain
            `name`, `offset`, `size` columns and MUST NOT contain
            `path`.
        validate: if True (default), re-runs the plan from `paths` and
            verifies that names, offsets, and sizes in the parquet
            match the plan. Set False only when the parquet is known
            correct and the read overhead is unwanted.

    Returns:
        Absolute path of the created archive.
    """
    out_path_str = str(Path(out_path).resolve())
    parquet_str = str(Path(metadata_parquet).resolve())

    if not Path(parquet_str).exists():
        raise FileNotFoundError(
            f"cozip: metadata parquet not found: {parquet_str}"
        )

    # Structural check is unconditional; enforces the contract.
    _check_parquet_schema(parquet_str)

    names, src_paths = _validate_paths_arg(paths)
    n_users = len(names)

    # Capacity = n_users + 2 leaves room for __metadata__ and the
    # optional padding slot that cozip_write_flat may append.
    entries, keepalive = _alloc_entries(names, src_paths, n_extra=2)
    err = ffi.new("cozip_error_t*")
    _check(lib.cozip_plan_flat(entries, n_users, err), err)

    if validate:
        planned_offsets = [int(entries[i].payload_offset) for i in range(n_users)]
        planned_sizes = [int(entries[i].payload_size) for i in range(n_users)]
        _validate_parquet_values(
            parquet_str, names, planned_offsets, planned_sizes
        )

    meta_path_c = ffi.new("char[]", parquet_str.encode("utf-8"))
    out_path_c = ffi.new("char[]", out_path_str.encode("utf-8"))
    keepalive.extend((meta_path_c, out_path_c))

    _check(
        lib.cozip_write_flat(
            out_path_c, entries, n_users, n_users + 2, meta_path_c, err
        ),
        err,
    )
    return out_path_str




def create(
    out_path: str | Path,
    table: pa.Table,
    temp_dir: str | Path | None = None,
) -> str:
    """All-in-one: stage_metadata + write parquet + stage_create.

    Args:
        out_path: destination cozip archive.
        table: pyarrow.Table with `name` and `path` columns. Extras
            preserved.
        temp_dir: directory for the temporary metadata parquet.
            Defaults to the system temp directory.

    Returns:
        Absolute path of the created archive.
    """
    metadata_table, paths = stage_metadata(table)

    tmp = _temp_parquet_path(temp_dir)
    try:
        pq.write_table(metadata_table, tmp)
        # validate=False because we just generated the parquet from the
        # same plan; redundant to re-validate against itself.
        return stage_create(out_path, paths, tmp, validate=False)
    finally:
        tmp.unlink(missing_ok=True)
