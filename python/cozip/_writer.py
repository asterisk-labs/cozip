"""High-level Flat-profile writer for path sources.

``create`` handles the usual one-call path. ``stage_metadata`` and
``stage_create`` expose the two stages when callers need to build the metadata
Parquet themselves.
"""

import os
import tempfile
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from ._core import (
    COZIP_SOURCE_PATH,
    CozipError,
    ffi,
    lib,
)
from ._parquet import manifest_options

# Reserved entry names; source of truth is libcozip.
_INDEX_NAME = ffi.string(lib.cozip_index_name()).decode("ascii")
_PADDING_NAME = ffi.string(lib.cozip_padding_name()).decode("ascii")
_METADATA_NAME = ffi.string(lib.cozip_flat_metadata_name()).decode("ascii")
_RESERVED_NAMES = frozenset({_INDEX_NAME, _METADATA_NAME, _PADDING_NAME})

# Columns owned by the writer or reader; producers must never persist them.
# They are compared without case, as DuckDB, which decodes manifests, does.
_PROTECTED_LOCATION_COLUMNS = frozenset({"cozip:location", "taco:location"})
_RESERVED_INPUT_COLUMNS = frozenset({"offset", "size"}) | _PROTECTED_LOCATION_COLUMNS

# Columns pandas adds for an unnamed index; never part of a manifest.
_PANDAS_INDEX_PREFIX = "__index_level_"

# Columns required in the metadata parquet that goes into the archive.
_REQUIRED_METADATA_COLUMNS = frozenset({"name", "offset", "size"})


def _check(status: int, err: Any) -> None:
    if status != 0:
        raise CozipError.from_struct(err)


def _path_text(value: Any, *, context: str) -> str:
    try:
        path = os.fspath(value)
    except TypeError as exc:
        raise TypeError(f"cozip: {context} must be path-like") from exc
    if not isinstance(path, str):
        raise TypeError(f"cozip: {context} must resolve to a string path")
    if not path:
        raise ValueError(f"cozip: {context} must be a non-empty path")
    if "\0" in path:
        raise ValueError(f"cozip: {context} contains a NUL byte")
    return path


def _validate_name_value(value: Any, *, context: str) -> str:
    if not isinstance(value, str):
        raise TypeError(f"cozip: {context} must be a string")
    if "\0" in value:
        raise ValueError(f"cozip: {context} contains a NUL byte")
    return value


def _check_column_names(names: list[str], *, what: str) -> None:
    """Column names must stay distinct when compared without case."""
    first: dict[str, str] = {}
    for name in names:
        other = first.setdefault(name.lower(), name)
        if other == name and names.count(name) > 1:
            raise ValueError(f"cozip: {what} has the column {name!r} more than once")
        if other != name:
            raise ValueError(
                f"cozip: {what} has columns that differ only in case: {other!r} "
                f"and {name!r}; DuckDB, which reads manifests, cannot tell them apart"
            )


def _validate_input_table(table: pa.Table) -> None:
    """Validate a (name, path, ...) input table for stage_metadata / create."""
    if not isinstance(table, pa.Table):
        hint = (
            "; convert it with pyarrow.Table.from_pandas(df, preserve_index=False)"
            if type(table).__name__ == "DataFrame"
            else ""
        )
        raise TypeError(
            f"cozip: table must be a pyarrow.Table, got {type(table).__name__}{hint}"
        )
    _check_column_names(table.column_names, what="input table")
    cols = set(table.column_names)
    missing = {"name", "path"} - cols
    if missing:
        raise ValueError(
            f"cozip: input table is missing required column(s): {sorted(missing)}"
        )
    reserved_in_input = sorted(
        c for c in table.column_names if c.lower() in _RESERVED_INPUT_COLUMNS
    )
    if reserved_in_input:
        raise ValueError(
            f"cozip: input table must not contain reserved column(s) "
            f"{reserved_in_input}; the binding computes them (names are "
            "compared without case)"
        )
    if len(table) == 0:
        raise ValueError("cozip: empty entry list")

    names = table.column("name").to_pylist()
    paths = table.column("path").to_pylist()

    seen: set[str] = set()
    for i, raw_name in enumerate(names):
        name = _validate_name_value(raw_name, context=f"row {i} name")
        if name in _RESERVED_NAMES:
            raise ValueError(f"cozip: row {i} uses reserved name {name!r}")
        if name in seen:
            raise ValueError(f"cozip: duplicate name {name!r} at row {i}")
        seen.add(name)

    for i, p in enumerate(paths):
        path = _path_text(p, context=f"row {i} path")
        source = Path(path)
        if not source.exists():
            raise FileNotFoundError(
                f"cozip: row {i} ({names[i]!r}): source not found: {path}"
            )
        if not source.is_file():
            raise ValueError(
                f"cozip: row {i} ({names[i]!r}): source is not a regular file: {path}"
            )


def _validate_paths_arg(
    paths: Sequence[tuple[str, str]],
) -> tuple[list[str], list[str]]:
    """Validate the (name, path) list passed to stage_create."""
    if isinstance(paths, (str, bytes)) or not isinstance(paths, Sequence):
        raise TypeError("cozip: paths must be a sequence of (name, source_path) pairs")
    if len(paths) == 0:
        raise ValueError("cozip: empty paths list")

    names: list[str] = []
    src_paths: list[str] = []
    for i, item in enumerate(paths):
        if (
            isinstance(item, (str, bytes))
            or not isinstance(item, Sequence)
            or len(item) != 2
        ):
            raise ValueError(f"cozip: paths[{i}] must be a (name, source_path) pair")
        name, path = item
        names.append(_validate_name_value(name, context=f"paths[{i}] name"))
        src_paths.append(_path_text(path, context=f"paths[{i}] source"))

    seen: set[str] = set()
    for i, name in enumerate(names):
        if name in _RESERVED_NAMES:
            raise ValueError(f"cozip: paths[{i}] uses reserved name {name!r}")
        if name in seen:
            raise ValueError(f"cozip: duplicate name {name!r} at paths[{i}]")
        seen.add(name)

    for i, p in enumerate(src_paths):
        source = Path(p)
        if not source.exists():
            raise FileNotFoundError(
                f"cozip: paths[{i}] ({names[i]!r}): source not found: {p}"
            )
        if not source.is_file():
            raise ValueError(
                f"cozip: paths[{i}] ({names[i]!r}): source is not a regular file: {p}"
            )

    return names, src_paths


def _alloc_entries(
    names: list[str],
    paths: list[str],
    n_extra: int,
    sizes: list[int] | None = None,
) -> tuple[Any, list[Any]]:
    """Allocate cozip_entry_t[len(names) + n_extra] with user slots filled

    Sizes come from `sizes` when given, else from a stat of each path.
    Returns (entries, keepalive). `keepalive` owns every cdata that
    libcozip reads through pointers in `entries` and must remain alive
    across every C call.
    """
    n = len(names)
    entries = ffi.new(f"cozip_entry_t[{n + n_extra}]")
    keepalive: list[Any] = []
    for i in range(n):
        name_c = ffi.new("char[]", names[i].encode("utf-8"))
        path_c = ffi.new("char[]", os.fsencode(paths[i]))
        keepalive.extend((name_c, path_c))
        entries[i].arc_name = name_c
        entries[i].payload_size = (
            sizes[i] if sizes is not None else os.stat(paths[i]).st_size
        )
        entries[i].in_index = False
        entries[i].source.kind = COZIP_SOURCE_PATH
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
    _check_column_names(schema.names, what="metadata parquet")
    cols = set(schema.names)

    if "path" in cols:
        raise ValueError(
            "cozip: metadata parquet must not contain a 'path' column. "
            "`path` is filesystem-local and does not belong inside the "
            "archive. Drop it before writing the parquet."
        )

    protected = sorted(c for c in cols if c.lower() in _PROTECTED_LOCATION_COLUMNS)
    if protected:
        raise ValueError(
            "cozip: metadata parquet must not contain reader-owned column(s): "
            f"{protected}"
        )

    missing = _REQUIRED_METADATA_COLUMNS - cols
    if missing:
        raise ValueError(
            f"cozip: metadata parquet is missing required column(s): {sorted(missing)}"
        )

    # Spec 13.3: name is a string, offset and size are uint64.
    name_type = schema.field("name").type
    if not (pa.types.is_string(name_type) or pa.types.is_large_string(name_type)):
        raise ValueError(
            f"cozip: metadata parquet column 'name' must be a string, got {name_type}"
        )
    for column in ("offset", "size"):
        column_type = schema.field(column).type
        if column_type != pa.uint64():
            raise ValueError(
                f"cozip: metadata parquet column {column!r} must be uint64 "
                f"(cozip spec 13.3), got {column_type}"
            )


def _validate_parquet_values(
    parquet_path: str,
    expected_names: list[str],
    expected_offsets: list[int],
    expected_sizes: list[int],
) -> None:
    """Compare metadata values with the plan using Arrow compute."""
    table = pq.read_table(parquet_path, columns=["name", "offset", "size"])

    n = len(table)
    if n != len(expected_names):
        raise ValueError(
            f"cozip: metadata parquet has {n} rows, paths has {len(expected_names)}"
        )

    # Build the expected Arrow arrays once.
    exp_names = pa.array(expected_names, type=pa.string())
    exp_off = pa.array(expected_offsets, type=pa.uint64())
    exp_sz = pa.array(expected_sizes, type=pa.uint64())

    # _check_parquet_schema has already required string and uint64 columns.
    pq_names = table.column("name")
    pq_off = table.column("offset")
    pq_sz = table.column("size")

    def _check(field: str, pq_col, exp_col, exp_list) -> None:
        if pq_col.null_count:
            i = pc.index(pc.is_null(pq_col), True).as_py()
            raise ValueError(f"cozip: metadata parquet has NULL {field} at row {i}")
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

    _check("name", pq_names, exp_names, expected_names)
    _check("offset", pq_off, exp_off, expected_offsets)
    _check("size", pq_sz, exp_sz, expected_sizes)


# Public API


def _plan(table: pa.Table) -> tuple[pa.Table, list[str], list[str], list[int]]:
    """Validate the input table, plan it, and build the manifest table.

    Returns (manifest table, names, source paths, planned sizes).
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

    # An unnamed pandas index becomes __index_level_N__ columns and a
    # 'pandas' schema key that would describe the dropped path column.
    dropped = ["path"] + [
        c for c in table.column_names if c.startswith(_PANDAS_INDEX_PREFIX)
    ]
    out = (
        table.drop_columns(dropped)
        .append_column("offset", pa.array(offsets, type=pa.uint64()))
        .append_column("size", pa.array(sizes, type=pa.uint64()))
    )
    metadata = dict(out.schema.metadata or {})
    metadata.pop(b"pandas", None)
    out = out.replace_schema_metadata(metadata or None)
    rest = [c for c in out.column_names if c not in ("name", "offset", "size")]
    return out.select(["name", "offset", "size", *rest]), names, src_paths, sizes


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

    The returned table and `paths` are aligned positionally.
    If the metadata table is reordered (e.g. through a DuckDB sort)
    before being written, `paths` MUST be reordered the same way, or
    `stage_create(validate=True)` will reject the mismatch.

    Args:
        table: pyarrow.Table with at least `name` (str) and `path`
            (str) columns. Extras are preserved in the returned table.
            Reserved columns `offset` or `size` in the input are
            rejected, compared without case.

    Returns:
        Tuple of:
          * pyarrow.Table with `name`, `offset`, `size`, then any user
            extras. `path` is dropped because filesystem paths do not
            belong inside the archive, and so are the columns pandas adds
            for an unnamed index.
          * list of (name, path) tuples, aligned with the table rows,
            ready to pass to `stage_create`.
    """
    metadata_table, names, src_paths, _sizes = _plan(table)
    return metadata_table, list(zip(names, src_paths))


def _write(
    out_path: str,
    names: list[str],
    src_paths: list[str],
    sizes: list[int] | None,
    parquet_str: str,
) -> str:
    """Write the archive around a checked manifest Parquet.

    With `sizes`, the entries keep the sizes planned earlier, and the C
    writer, which stats every source, refuses a file that changed since.
    """
    n_users = len(names)
    # Capacity = n_users + 2 leaves room for __metadata__ and the
    # optional padding slot that cozip_write_flat may append.
    entries, keepalive = _alloc_entries(names, src_paths, n_extra=2, sizes=sizes)
    err = ffi.new("cozip_error_t*")
    _check(lib.cozip_plan_flat(entries, n_users, err), err)
    meta_path_c = ffi.new("char[]", os.fsencode(parquet_str))
    out_path_c = ffi.new("char[]", os.fsencode(out_path))
    keepalive.extend((meta_path_c, out_path_c))
    _check(
        lib.cozip_write_flat(
            out_path_c, entries, n_users, n_users + 2, meta_path_c, err
        ),
        err,
    )
    return out_path


def stage_create(
    out_path: str | Path,
    paths: Sequence[tuple[str, str]],
    metadata_parquet: str | Path,
    validate: bool = True,
) -> str:
    """Pack a cozip archive from source files and a user-written parquet.

    The metadata parquet is embedded verbatim as the `__metadata__`
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
            match the plan. Set False when the parquet is known to be
            correct and the read overhead is unwanted.

    Returns:
        Absolute path of the created archive.
    """
    if not isinstance(validate, bool):
        raise TypeError("cozip: validate must be a boolean")

    out_path_str = str(Path(_path_text(out_path, context="out_path")).resolve())
    parquet_str = str(
        Path(_path_text(metadata_parquet, context="metadata_parquet")).resolve()
    )

    parquet_path = Path(parquet_str)
    if not parquet_path.exists():
        raise FileNotFoundError(f"cozip: metadata parquet not found: {parquet_str}")
    if not parquet_path.is_file():
        raise ValueError(
            f"cozip: metadata parquet is not a regular file: {parquet_str}"
        )
    if parquet_path.stat().st_size == 0:
        raise ValueError("cozip: metadata parquet is empty")

    # Structural check is unconditional; enforces the contract.
    _check_parquet_schema(parquet_str)

    names, src_paths = _validate_paths_arg(paths)
    n_users = len(names)

    sizes = None
    if validate:
        entries, _keepalive = _alloc_entries(names, src_paths, n_extra=1)
        err = ffi.new("cozip_error_t*")
        _check(lib.cozip_plan_flat(entries, n_users, err), err)
        sizes = [int(entries[i].payload_size) for i in range(n_users)]
        planned_offsets = [int(entries[i].payload_offset) for i in range(n_users)]
        _validate_parquet_values(parquet_str, names, planned_offsets, sizes)

    return _write(out_path_str, names, src_paths, sizes, parquet_str)


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
    out_path_str = str(Path(_path_text(out_path, context="out_path")).resolve())
    metadata_table, names, src_paths, sizes = _plan(table)

    tmp = _temp_parquet_path(temp_dir)
    try:
        pq.write_table(metadata_table, tmp, **manifest_options(metadata_table))
        # The manifest comes from this plan, so it is written with the
        # planned sizes: one stat per source here, and one in the C writer,
        # which refuses a source that changed size in between.
        return _write(out_path_str, names, src_paths, sizes, str(tmp))
    finally:
        tmp.unlink(missing_ok=True)
