"""Read the manifest of a Flat-profile cozip archive."""

import os
import tempfile
import threading
from collections.abc import Sequence

import pandas as pd

from ._verify import check_source


_LOCATION_COLUMN = "cozip:location"
# Columns a reader computes. A stored column with one of these names is
# dropped in any case: DuckDB compares names without case, so a stored
# 'COZIP:LOCATION' would otherwise pass for the location (spec 12.3).
_READER_OWNED_COLUMNS = frozenset({_LOCATION_COLUMN, "taco:location", "cozip:gdal_vsi"})
_REQUIRED_COLUMNS = ("name", "offset", "size")

# DuckDB only decodes the fetched manifest from a local file, so it needs no
# extension and must never install or load one.
_DUCKDB_CONFIG = {
    "autoinstall_known_extensions": False,
    "autoload_known_extensions": False,
}

# One in-memory DuckDB database per process; each read takes its own cursor.
# DuckDB is not fork-safe, so a child process opens a database of its own.
_DB = None
_DB_PID = None
_DB_LOCK = threading.Lock()


def _cursor():
    import duckdb

    global _DB, _DB_PID
    with _DB_LOCK:
        if _DB is None or _DB_PID != os.getpid():
            _DB = duckdb.connect(config=_DUCKDB_CONFIG)
            _DB_PID = os.getpid()
        return _DB.cursor()


def _quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def _stored_columns(con, metadata: str) -> list[str]:
    """Top-level column names of the manifest, spelled as stored."""
    rows = con.execute(
        "SELECT name, num_children FROM parquet_schema(?)", [metadata]
    ).fetchall()
    names: list[str] = []
    i = 1
    for _ in range(rows[0][1] or 0):
        names.append(rows[i][0])
        # Skip the subtree of a nested column.
        pending = 1
        while pending:
            pending += (rows[i][1] or 0) - 1
            i += 1
    return names


def _manifest_columns(
    stored: list[str], columns: Sequence[str] | None
) -> tuple[list[str], list[str]]:
    """The columns to return around the location: (before, after)."""
    kept = [c for c in stored if c.lower() not in _READER_OWNED_COLUMNS]
    missing = [c for c in _REQUIRED_COLUMNS if c not in kept]
    if missing:
        raise ValueError(f"cozip.read: the manifest lacks required column(s) {missing}")
    first: dict[str, str] = {}
    for column in kept:
        other = first.setdefault(column.lower(), column)
        if other != column:
            raise ValueError(
                "cozip.read: the manifest has columns that differ only in case: "
                f"{other!r} and {column!r}; DuckDB, which decodes the manifest, "
                "cannot tell them apart"
            )
    if columns is None:
        return kept, []
    extras: list[str] = []
    unknown: list[str] = []
    for column in columns:
        if column.lower() in _READER_OWNED_COLUMNS:
            continue
        if column in _REQUIRED_COLUMNS or column in extras:
            continue
        if column not in kept:
            unknown.append(column)
            continue
        extras.append(column)
    if unknown:
        raise ValueError(
            f"cozip.read: unknown column(s) {unknown}; the manifest has {kept}"
        )
    return list(_REQUIRED_COLUMNS), extras


def _fetch_metadata(source: str, destination: str) -> None:
    """Writes the archive's __metadata__ Parquet bytes to destination."""
    from ._core import CozipError, ffi, lib

    data = ffi.new("uint8_t **")
    size = ffi.new("size_t *")
    message = ffi.new("char **")
    status = lib.cozip_read_flat_metadata(source.encode("ascii"), data, size, message)
    try:
        if status != 0:
            raise CozipError.from_message(status, message[0])
        with open(destination, "wb") as stream:
            stream.write(ffi.buffer(data[0], size[0]))
    finally:
        lib.cozip_free(data[0])
        lib.cozip_free(message[0])


def _location_base(source: str) -> str:
    """The GDAL path that cozip:location appends after the byte range."""
    from ._core import CozipError, ffi, lib

    base = ffi.new("char **")
    message = ffi.new("char **")
    status = lib.cozip_location_base(source.encode("ascii"), base, message)
    try:
        if status != 0:
            raise CozipError.from_message(status, message[0])
        return ffi.string(base[0]).decode("ascii")
    finally:
        lib.cozip_free(base[0])
        lib.cozip_free(message[0])


def _read_flat_archive(
    source: str, columns: Sequence[str] | None, location: bool
) -> pd.DataFrame:
    import duckdb

    with tempfile.TemporaryDirectory(prefix="cozip-") as directory:
        metadata = os.path.join(directory, "__metadata__.parquet")
        _fetch_metadata(source, metadata)
        base = _location_base(source) if location else ""
        con = _cursor()
        try:
            before, after = _manifest_columns(_stored_columns(con, metadata), columns)
            parts = [_quote_identifier(column) for column in before]
            params = [metadata]
            if location:
                # The body of the read_flat macro in cozip_reader.
                parts.append(
                    "'/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ? "
                    f"AS {_quote_identifier(_LOCATION_COLUMN)}"
                )
                params = [base, metadata]
            parts.extend(_quote_identifier(column) for column in after)
            # Hive partitioning stays off so the manifest's path never adds columns.
            sql = (
                f"SELECT {', '.join(parts)} "
                "FROM read_parquet(?, hive_partitioning = false)"
            )
            return con.execute(sql, params).df()
        except duckdb.Error as exc:
            text = str(exc)
            if metadata not in text:
                raise
            label = source if "://" not in source else "remote archive"
            raise type(exc)(text.replace(metadata, f"{label}!__metadata__")) from None
        finally:
            con.close()


def read(
    source: str | os.PathLike[str],
    columns: Sequence[str] | None = None,
    location: bool = True,
) -> pd.DataFrame:
    """Read the manifest of a Flat-profile cozip archive.

    ``columns`` selects extra manifest columns by exact name; the required
    ``name``, ``offset``, and ``size`` columns remain, and an unknown name
    raises ``ValueError``. Set ``location=False`` to omit the synthetic
    ``cozip:location`` column. Archives using any profile other than Flat
    (profile 1), including TACO (profile 2), are rejected with a
    ``CozipError``.
    """
    source = check_source(source, "cozip.read")
    if isinstance(columns, (str, bytes)) or (
        columns is not None
        and (
            not isinstance(columns, Sequence)
            or any(not isinstance(column, str) or not column for column in columns)
        )
    ):
        raise TypeError("cozip.read: columns must be a sequence of non-empty strings")
    if not isinstance(location, bool):
        raise TypeError("cozip.read: location must be a boolean")
    return _read_flat_archive(source, columns, location)
