"""Pure-Python reader for cozip 1.0 archives and their INDEX.parquet
catalogs. Local or remote.

Two modes, picked by the source name:

  * `*.zip`         → cozip mode. Bootstraps the cozip index in one
                       speculative 64 KiB range request, fetches the
                       __metadata__ parquet, returns it with the
                       injected `cozip:gdal_vsi` column.

  * `INDEX.parquet` → catalog mode. Fetches the parquet directly,
                       validates required columns (name, offset, size,
                       shard), and builds `cozip:gdal_vsi` per row by
                       resolving the `shard` column against the
                       INDEX's parent directory.

No dependency on libcozip. pandas is imported lazily.
"""

import io
import posixpath
import struct
from pathlib import Path
from urllib.request import Request, urlopen

# Constants from the cozip 1.0 spec (see cozip.h).
_INDEX_OFFSET    = 51
_INDEX_NAME      = b"__cozip__"
_MAGIC           = b"CZIP"
_HEADER_SIZE     = 11
_ZIP_SIG         = b"PK\x03\x04"
_BOOTSTRAP_SIZE  = 65536  # 64 KiB

_REQUIRED_SHARD   = ("name", "offset", "size")
_REQUIRED_CATALOG = ("name", "offset", "size", "shard")

_CATALOG_FILENAME = "INDEX.parquet"


# ----------------------------------------------------------------
# I/O primitive
# ----------------------------------------------------------------

def _is_url(s: str) -> bool:
    return s.startswith(("http://", "https://"))


def _get(source: str, offset: int, size: int, *, is_remote: bool) -> bytes:
    """Range-read primitive. Local seek+read, or HTTP Range."""
    if is_remote:
        req = Request(
            source,
            headers={
                "Range": f"bytes={offset}-{offset + size - 1}",
                "User-Agent": "Mozilla/5.0",
            },
        )
        with urlopen(req) as r:  # noqa: S310 (controlled URL)
            status = getattr(r, "status", 200)
            data = r.read()
        if status == 200:
            data = data[offset : offset + size]
        elif len(data) > size:
            data = data[:size]
        if len(data) < size:
            raise IOError(
                f"short read at offset {offset}: got {len(data)}, "
                f"expected {size}"
            )
        return data

    with open(source, "rb") as f:
        f.seek(offset)
        data = f.read(size)
    if len(data) < size:
        raise IOError(
            f"short read at offset {offset}: got {len(data)}, "
            f"expected {size} (truncated file?)"
        )
    return data


def _get_all(source: str, *, is_remote: bool) -> bytes:
    """Fetch the entire source. Used for INDEX.parquet (catalog mode)."""
    if is_remote:
        req = Request(source, headers={"User-Agent": "Mozilla/5.0"})
        with urlopen(req) as r:  # noqa: S310
            return r.read()
    with open(source, "rb") as f:
        return f.read()


# ----------------------------------------------------------------
# Cozip index parsing
# ----------------------------------------------------------------

def _parse_index(payload: bytes) -> dict[str, tuple[int, int]]:
    """Parse the cozip index payload into {name: (offset, size)}."""
    if payload[:4] != _MAGIC:
        raise ValueError(f"bad index magic: {payload[:4]!r}")
    n = struct.unpack_from("<I", payload, 7)[0]

    cur = _HEADER_SIZE
    name_lens = struct.unpack_from(f"<{n}H", payload, cur)
    cur += 2 * n

    names = []
    for nl in name_lens:
        names.append(payload[cur : cur + nl].decode("utf-8"))
        cur += nl

    offsets = struct.unpack_from(f"<{n}Q", payload, cur)
    cur += 8 * n
    sizes = struct.unpack_from(f"<{n}Q", payload, cur)

    return {nm: (off, sz) for nm, off, sz in zip(names, offsets, sizes)}


def _bootstrap_cozip(source: str, *, is_remote: bool) -> dict[str, tuple[int, int]]:
    """Pull the head, validate the LFH, parse the index. One request
    in the common case, two if the index doesn't fit in 64 KiB.
    """
    head = _get(source, 0, _BOOTSTRAP_SIZE, is_remote=is_remote)

    if len(head) < _INDEX_OFFSET:
        raise ValueError(f"archive too small: {len(head)} bytes")
    if head[:4] != _ZIP_SIG:
        raise ValueError("not a ZIP archive (bad PK\\x03\\x04 signature)")
    if head[30:39] != _INDEX_NAME:
        raise ValueError("first ZIP entry is not __cozip__")

    index_size = struct.unpack_from("<I", head, 18)[0]
    if index_size == 0 or index_size == 0xFFFFFFFF:
        raise ValueError(f"bad index_payload_size in LFH: {index_size}")

    end = _INDEX_OFFSET + index_size
    if end > len(head):
        rest = _get(source, len(head), end - len(head), is_remote=is_remote)
        head += rest

    return _parse_index(head[_INDEX_OFFSET:end])


# ----------------------------------------------------------------
# VSI helpers
# ----------------------------------------------------------------

def _vsi_base_zip(source: str, *, is_remote: bool) -> str:
    """Right-hand side for a single .zip source."""
    return f"/vsicurl/{source}" if is_remote else str(Path(source).resolve())


def _vsi_base_dir(source: str, *, is_remote: bool) -> str:
    """Parent directory of an INDEX.parquet, with trailing slash.
    Used as a prefix to which each row's `shard` is appended.
    """
    if is_remote:
        # posixpath handles URLs correctly (always /).
        parent = posixpath.dirname(source)
        return f"/vsicurl/{parent}/"
    return str(Path(source).resolve().parent) + "/"


def _check_required(df, required: tuple[str, ...], mode: str) -> None:
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise ValueError(
            f"{mode} parquet is missing required column(s) {missing}; "
            f"have {list(df.columns)}"
        )


# ----------------------------------------------------------------
# Public API
# ----------------------------------------------------------------

def read(source: str):
    """Read a cozip shard or an INDEX.parquet catalog as a DataFrame.

    Mode is picked by the source name:

    * `*.zip` (a cozip archive) → loads its `__metadata__` parquet.
      Returns the parquet verbatim plus a `cozip:gdal_vsi` column
      with one /vsisubfile path per row, all anchored to the same
      .zip file.

    * `INDEX.parquet` (a catalog of multiple shards) → loads the
      parquet directly. Each row must declare which shard it lives
      in via the `shard` column, and the `cozip:gdal_vsi` path is
      built by resolving `shard` against the INDEX's parent dir.

    Args:
        source: Local filesystem path or http(s) URL ending in
            `.zip` or `INDEX.parquet`.

    Returns:
        pandas.DataFrame with the original parquet columns plus
        `cozip:gdal_vsi`.

    Raises:
        ImportError: pandas is not installed.
        ValueError: source name is unrecognised, the archive is
            malformed, or required columns are missing.
        IOError: the read failed (truncated file, server error, etc).
    """
    try:
        import pandas as pd
    except ImportError as e:
        raise ImportError(
            "cozip.read requires pandas. Install with `pip install pandas`."
        ) from e

    is_remote = _is_url(source)

    # --- Catalog mode: INDEX.parquet -----------------------------
    if source.endswith(_CATALOG_FILENAME):
        payload = _get_all(source, is_remote=is_remote)
        df = pd.read_parquet(io.BytesIO(payload))

        _check_required(df, _REQUIRED_CATALOG, "INDEX.parquet")

        base = _vsi_base_dir(source, is_remote=is_remote)
        df["cozip:gdal_vsi"] = (
            "/vsisubfile/"
            + df["offset"].astype(str)
            + "_"
            + df["size"].astype(str)
            + ","
            + base
            + df["shard"].astype(str)
        )
        return df

    # --- Cozip mode: *.zip ---------------------------------------
    if source.endswith(".zip"):
        index = _bootstrap_cozip(source, is_remote=is_remote)

        if "__metadata__" not in index:
            raise ValueError(
                "archive has no __metadata__ entry "
                "(not a FLAT-profile cozip?). "
                f"Priority entries: {sorted(index)}"
            )

        offset, size = index["__metadata__"]
        payload = _get(source, offset, size, is_remote=is_remote)
        df = pd.read_parquet(io.BytesIO(payload))

        _check_required(df, _REQUIRED_SHARD, "__metadata__")

        base = _vsi_base_zip(source, is_remote=is_remote)
        df["cozip:gdal_vsi"] = (
            "/vsisubfile/"
            + df["offset"].astype(str)
            + "_"
            + df["size"].astype(str)
            + ","
            + base
        )
        return df

    raise ValueError(
        f"unrecognised source: {source!r}. "
        f"Must end in '.zip' or '{_CATALOG_FILENAME}'."
    )