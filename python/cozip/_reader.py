"""Read the manifest of a Flat-profile cozip archive."""

import os
from collections.abc import Sequence

import pandas as pd


_LOCATION_COLUMN = "cozip:location"
_LEGACY_LOCATION_COLUMN = "cozip:gdal_vsi"
_PROTECTED_LOCATION_COLUMNS = frozenset({_LOCATION_COLUMN, "taco:location"})


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


def _projection(
    columns: Sequence[str] | None,
    location: bool,
    source_location_column: str = _LOCATION_COLUMN,
) -> str:
    selected = _selected_columns(columns, location)
    if selected is None:
        return "*"
    expressions = []
    for column in selected:
        source = (
            source_location_column if column == _LOCATION_COLUMN else column
        )
        expression = _quote_identifier(source)
        if source != column:
            expression += f" AS {_quote_identifier(column)}"
        expressions.append(expression)
    return ", ".join(expressions)


def _legacy_flat_signature(message: str) -> bool:
    return (
        "read_flat" in message
        and "gdal_vsi" in message
        and "does not support the supplied arguments" in message
    )


def _missing_read_flat(message: str) -> bool:
    return "read_flat" in message and "does not exist" in message


def _read_flat_archive(
    source: str, columns: Sequence[str] | None, location: bool
) -> pd.DataFrame:
    import duckdb

    local_extension = os.environ.get("COZIP_EXTENSION")
    if local_extension:
        con = duckdb.connect(config={"allow_unsigned_extensions": True})
    else:
        con = duckdb.connect()
    try:
        if local_extension:
            con.load_extension(local_extension)
        else:
            con.execute("INSTALL cozip FROM community; LOAD cozip;")
        enabled = "true" if location else "false"
        sql = (
            f"SELECT {_projection(columns, location)} "
            f"FROM read_flat(?, location := {enabled})"
        )
        legacy = False
        try:
            result = con.execute(sql, [source])
        except Exception as exc:
            message = str(exc)
            if _legacy_flat_signature(message):
                legacy_sql = (
                    f"SELECT {_projection(columns, location, _LEGACY_LOCATION_COLUMN)} "
                    f"FROM read_flat(?, gdal_vsi := {enabled})"
                )
            elif _missing_read_flat(message):
                legacy_sql = (
                    f"SELECT {_projection(columns, location, _LEGACY_LOCATION_COLUMN)} "
                    f"FROM read_cozip(?, gdal_vsi := {enabled})"
                )
            else:
                raise
            result = con.execute(legacy_sql, [source])
            legacy = True
        df = result.df()
        protected = [
            column
            for column in _PROTECTED_LOCATION_COLUMNS
            if column in df.columns
        ]
        if legacy and location:
            if protected:
                df = df.drop(columns=protected)
        elif "taco:location" in protected:
            df = df.drop(columns=["taco:location"])
        if legacy and location and _LEGACY_LOCATION_COLUMN in df.columns:
            df = df.rename(columns={_LEGACY_LOCATION_COLUMN: _LOCATION_COLUMN})
        if not location:
            synthetic = [
                column
                for column in (_LOCATION_COLUMN, _LEGACY_LOCATION_COLUMN)
                if column in df.columns
            ]
            if synthetic:
                df = df.drop(columns=synthetic)
        return df
    finally:
        con.close()


def read(
    source: str | os.PathLike[str],
    columns: Sequence[str] | None = None,
    location: bool = True,
) -> pd.DataFrame:
    """Read the manifest of a Flat-profile cozip archive.

    ``columns`` selects extra manifest columns; the required ``name``,
    ``offset``, and ``size`` columns remain. Set ``location=False`` to omit
    the synthetic ``cozip:location`` column. Archives using any profile other
    than Flat (profile 1), including TACO (profile 2), are rejected by
    ``read_flat``.
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
