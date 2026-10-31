"""Read the manifest of a Flat-profile cozip archive."""

import os
import tempfile
from collections.abc import Sequence

import pandas as pd


_LOCATION_COLUMN = "cozip:location"
_PROTECTED_LOCATION_COLUMNS = frozenset({_LOCATION_COLUMN, "taco:location"})

# DuckDB only decodes the fetched manifest from a local file, so it needs no
# extension and must never install or load one.
_DUCKDB_CONFIG = {
    "autoinstall_known_extensions": False,
    "autoload_known_extensions": False,
}

# The body of the read_flat macro in cozip_reader, over the fetched manifest.
# Hive partitioning stays off so the manifest's path never adds columns.
_MANIFEST_SQL = (
    "SELECT COLUMNS(lambda c: c != 'cozip:location' AND c != 'cozip:gdal_vsi'), "
    "CASE WHEN {location} "
    "THEN '/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ? "
    'ELSE NULL END AS "cozip:location" '
    "FROM read_parquet(?, hive_partitioning = false)"
)


def _quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def _selected_columns(
    columns: Sequence[str] | None, location: bool
) -> list[str] | None:
    if columns is None:
        return None
    required = ["name", "offset", "size"]
    if location:
        required.append(_LOCATION_COLUMN)
    extras = [
        column
        for column in columns
        if column not in _PROTECTED_LOCATION_COLUMNS
    ]
    return list(dict.fromkeys([*required, *extras]))


def _projection(columns: Sequence[str] | None, location: bool) -> str:
    selected = _selected_columns(columns, location)
    if selected is None:
        return "*"
    return ", ".join(_quote_identifier(column) for column in selected)


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
        sql = (
            f"SELECT {_projection(columns, location)} FROM ("
            f"{_MANIFEST_SQL.format(location='true' if location else 'false')})"
        )
        con = duckdb.connect(config=_DUCKDB_CONFIG)
        try:
            df = con.execute(sql, [base, metadata]).df()
        except duckdb.Error as exc:
            text = str(exc)
            if metadata not in text:
                raise
            label = source if "://" not in source else "remote archive"
            raise type(exc)(text.replace(metadata, f"{label}!__metadata__")) from None
        finally:
            con.close()
    if "taco:location" in df.columns:
        df = df.drop(columns=["taco:location"])
    if not location and _LOCATION_COLUMN in df.columns:
        df = df.drop(columns=[_LOCATION_COLUMN])
    return df


def read(
    source: str | os.PathLike[str],
    columns: Sequence[str] | None = None,
    location: bool = True,
) -> pd.DataFrame:
    """Read the manifest of a Flat-profile cozip archive.

    ``columns`` selects extra manifest columns; the required ``name``,
    ``offset``, and ``size`` columns remain. Set ``location=False`` to omit
    the synthetic ``cozip:location`` column. Archives using any profile other
    than Flat (profile 1), including TACO (profile 2), are rejected with a
    ``CozipError``.
    """
    source = os.fspath(source)
    if not isinstance(source, str):
        raise TypeError("cozip.read: source must be a string path or URL")
    if not source:
        raise ValueError("cozip.read: source must be a non-empty ASCII path or URL")
    if "\0" in source:
        raise ValueError("cozip.read: source contains a NUL byte")
    if not source.isascii():
        raise ValueError(
            "cozip.read: source contains non-ASCII characters; "
            "cozip supports ASCII paths and URLs only"
        )
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
