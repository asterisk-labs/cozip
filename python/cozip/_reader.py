"""Read a cozip 1.0 FLAT-profile archive's manifest.

Thin wrapper around the DuckDB cozip extension: hands the source to
read_cozip(), returns the result as a pandas.DataFrame. For filtering,
joins, or any non-trivial query, use DuckDB directly.
"""

import pandas as pd


_REQUIRED_COLUMNS = ("name", "offset", "size")


def _quote(col: str) -> str:
    """SQL identifier quoting. Handles colons in names like cozip:gdal_vsi."""
    return '"' + col.replace('"', '""') + '"'


def _build_select(columns: list[str] | None, gdal_vsi: bool) -> str:
    if columns is None:
        return "*"

    required = list(_REQUIRED_COLUMNS)
    if gdal_vsi:
        required.append("cozip:gdal_vsi")

    seen = set(required)
    ordered = list(required)
    for c in columns:
        if c not in seen:
            ordered.append(c)
            seen.add(c)

    return ", ".join(_quote(c) for c in ordered)


def read(
    source: str,
    *,
    columns: list[str] | None = None,
    gdal_vsi: bool = True,
) -> pd.DataFrame:
    """Read the manifest of a FLAT-profile cozip archive.

    Args:
        source: local path or http(s)/s3/gcs/azure/hf URL to the .zip.
        columns: extra columns to bring. `name`, `offset`, `size`
            (and `cozip:gdal_vsi` when gdal_vsi=True) are always
            included. None returns every column in the manifest.
        gdal_vsi: include the `cozip:gdal_vsi` column (one /vsisubfile
            path per row, ready to feed to GDAL).

    Returns:
        pandas.DataFrame.

    For projection beyond extras + filtering + custom SQL, use DuckDB
    directly:

        import duckdb
        con = duckdb.connect()
        con.execute("INSTALL cozip FROM community; LOAD cozip;")
        con.sql("SELECT name, offset FROM read_cozip(?) WHERE split = 'train'",
                [path]).df()
    """
    import duckdb

    con = duckdb.connect()
    con.execute("INSTALL cozip FROM community; LOAD cozip;")

    select = _build_select(columns, gdal_vsi)
    gdal_vsi_lit = "true" if gdal_vsi else "false"
    sql = f"SELECT {select} FROM read_cozip(?, gdal_vsi := {gdal_vsi_lit})"
    return con.execute(sql, [source]).df()