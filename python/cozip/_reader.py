"""Read the manifest of a Flat-profile cozip archive."""

import os
from collections.abc import Sequence

import pandas as pd


def _quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def _selected_columns(
    columns: Sequence[str] | None, gdal_vsi: bool
) -> list[str] | None:
    if columns is None:
        return None
    required = ["name", "offset", "size"]
    if gdal_vsi:
        required.append("cozip:gdal_vsi")
    extras = [column for column in columns if gdal_vsi or column != "cozip:gdal_vsi"]
    return list(dict.fromkeys([*required, *extras]))


def _read_flat_archive(
    source: str, columns: Sequence[str] | None, gdal_vsi: bool
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
        selected = _selected_columns(columns, gdal_vsi)
        if selected is None:
            projection = "*"
        else:
            projection = ", ".join(map(_quote_identifier, selected))
        enabled = "true" if gdal_vsi else "false"
        sql = f"SELECT {projection} FROM read_flat(?, gdal_vsi := {enabled})"
        try:
            result = con.execute(sql, [source])
        except Exception as exc:
            message = str(exc)
            if "read_flat" not in message or "does not exist" not in message:
                raise
            legacy_sql = sql.replace("FROM read_flat(", "FROM read_cozip(", 1)
            result = con.execute(legacy_sql, [source])
        df = result.df()
        if selected is None and not gdal_vsi and "cozip:gdal_vsi" in df.columns:
            df = df.drop(columns=["cozip:gdal_vsi"])
        return df
    finally:
        con.close()


def read(
    source: str | os.PathLike[str],
    columns: Sequence[str] | None = None,
    gdal_vsi: bool = True,
) -> pd.DataFrame:
    """Read the manifest of a Flat-profile cozip archive.

    ``columns`` selects extra manifest columns; the required ``name``,
    ``offset``, and ``size`` columns remain. Set ``gdal_vsi=False`` to omit
    the synthetic ``cozip:gdal_vsi`` column. Archives using any profile other
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
    if not isinstance(gdal_vsi, bool):
        raise TypeError("cozip.read: gdal_vsi must be a boolean")
    return _read_flat_archive(source, columns, gdal_vsi)
