"""Read a cozip archive (.zip) or catalog parquet (.parquet)."""

import os

import pandas as pd


def _is_parquet(source: str) -> bool:
    path = source.split("?", 1)[0].split("#", 1)[0]
    return path.lower().endswith(".parquet")


def _dirname(source: str) -> str:
    bare = source.split("?", 1)[0].split("#", 1)[0]
    i = bare.rfind("/")
    return bare[:i] if i >= 0 else ""


def _zip_url(base: str, shard: str) -> str:
    if shard.lower().startswith(("http://", "https://", "s3://", "gs://", "hf://")):
        return shard
    if not shard.lower().endswith(".zip"):
        shard = shard + ".zip"
    return f"{base}/{shard}" if base else shard


def _vsisubfile(url: str, offset: int, size: int) -> str:
    inner = f"/vsicurl/{url}" if url.lower().startswith(("http://", "https://")) else url
    return f"/vsisubfile/{offset}_{size},{inner}"


def _read_zip(source: str) -> pd.DataFrame:
    import duckdb

    con = duckdb.connect()
    con.execute("INSTALL cozip FROM community; LOAD cozip;")
    return con.execute(
        "SELECT * FROM read_cozip(?, gdal_vsi := true)", [source]
    ).df()


def _read_catalog(source: str) -> pd.DataFrame:
    import duckdb

    con = duckdb.connect()
    df = con.execute("SELECT * FROM read_parquet(?)", [source]).df()

    if "cozip:gdal_vsi" in df.columns:
        return df

    missing = [c for c in ("shard", "offset", "size") if c not in df.columns]
    if missing:
        raise ValueError(
            f"cozip.read: catalog {source!r} missing column(s) {missing}; "
            f"cannot synthesize cozip:gdal_vsi"
        )

    base = _dirname(source)
    df["cozip:gdal_vsi"] = [
        _vsisubfile(_zip_url(base, str(s)), int(o), int(sz))
        for s, o, sz in zip(df["shard"], df["offset"], df["size"])
    ]
    return df


def read(source: str | os.PathLike[str]) -> pd.DataFrame:
    """Read a cozip archive (.zip) or catalog parquet (.parquet).

    Output always includes a `cozip:gdal_vsi` column.
    """
    source = os.fspath(source)
    if _is_parquet(source):
        return _read_catalog(source)
    return _read_zip(source)