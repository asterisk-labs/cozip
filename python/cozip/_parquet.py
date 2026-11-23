"""Parquet options for the manifest that ``create`` writes.

The manifest is compressed with zstd, and each column gets the encoding that
writes its first rows smallest: dictionary, plain, byte stream split or delta.
Readers in every binding decode all four, hyparquet included.
"""

import io
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

ZSTD_LEVEL = 9
ENCODINGS = ("dictionary", "plain", "byte_stream_split", "delta")
_FLOATS = frozenset({"FLOAT", "DOUBLE"})
_INTEGERS = frozenset({"INT32", "INT64"})
_TRIAL_ROWS = 10_000
_BASE = {"compression": "zstd", "compression_level": ZSTD_LEVEL, "write_statistics": True}


def _leaves(field: pa.Field) -> list[tuple[str, str]]:
    """Parquet leaf paths and physical types of one Arrow field."""
    buffer = io.BytesIO()
    pq.write_table(pa.schema([field]).empty_table(), buffer)
    buffer.seek(0)
    schema = pq.ParquetFile(buffer).schema
    return [(schema.column(i).path, schema.column(i).physical_type) for i in range(len(schema))]


def _plan(encoding: str, leaves: list[tuple[str, str]]) -> tuple[list[str], dict[str, str]]:
    """Dictionary columns and per-column encodings for one strategy."""
    if encoding == "dictionary":
        return [path for path, kind in leaves if kind != "BOOLEAN"], {}
    encodings = {}
    for path, kind in leaves:
        if kind == "BOOLEAN":
            continue
        if encoding == "byte_stream_split" and kind in _FLOATS:
            encodings[path] = "BYTE_STREAM_SPLIT"
        elif encoding == "delta" and kind in _INTEGERS:
            encodings[path] = "DELTA_BINARY_PACKED"
        # Not DELTA_BYTE_ARRAY: hyparquet reads it only in version 2 data pages.
        elif encoding == "delta" and kind == "BYTE_ARRAY":
            encodings[path] = "DELTA_LENGTH_BYTE_ARRAY"
        else:
            encodings[path] = "PLAIN"
    return [], encodings


def _has_dictionary(dtype: pa.DataType) -> bool:
    if pa.types.is_dictionary(dtype):
        return True
    if pa.types.is_struct(dtype):
        return any(_has_dictionary(field.type) for field in dtype)
    if pa.types.is_map(dtype):
        return _has_dictionary(dtype.key_type) or _has_dictionary(dtype.item_type)
    if pa.types.is_list(dtype) or pa.types.is_large_list(dtype) or pa.types.is_fixed_size_list(dtype):
        return _has_dictionary(dtype.value_type)
    return False


def _smallest(column: pa.Table, leaves: list[tuple[str, str]]) -> str:
    # pyarrow cannot read a categorical column back from the other two.
    candidates = ENCODINGS[:2] if _has_dictionary(column.schema[0].type) else ENCODINGS
    sizes = []
    for encoding in candidates:
        dictionary, encodings = _plan(encoding, leaves)
        buffer = io.BytesIO()
        pq.write_table(column, buffer, **_BASE, use_dictionary=dictionary, column_encoding=encodings)
        sizes.append((buffer.tell(), ENCODINGS.index(encoding), encoding))
    return min(sizes)[2]


def manifest_options(table: pa.Table) -> dict[str, Any]:
    """``pq.write_table`` keyword arguments for a manifest table."""
    sample = table.slice(0, _TRIAL_ROWS)
    dictionary: list[str] = []
    encodings: dict[str, str] = {}
    for field in table.schema:
        leaves = _leaves(field)
        encoding = _smallest(sample.select([field.name]), leaves)
        column_dictionary, column_encodings = _plan(encoding, leaves)
        dictionary.extend(column_dictionary)
        encodings.update(column_encodings)
    return {**_BASE, "use_dictionary": dictionary, "column_encoding": encodings}
