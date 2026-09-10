"""Conformance tests for the cozip Python writer"""

import io
import struct
import sys
import zipfile
from itertools import pairwise
from pathlib import Path
from types import SimpleNamespace

import cozip
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

SMALL_CONTENT = b"hello cozip\n" * 8
MEDIUM_CONTENT = bytes(range(256)) * 160

INDEX_OFFSET = 51
HASH_WINDOW = 32768
ZIP_LFH_SIG = b"PK\x03\x04"
INDEX_NAME = b"__cozip__"
HASH_BLOCK_HEADER = b"\x0c\xca\x08\x00"
INDEX_MAGIC = b"CZIP"
INDEX_VERSION = b"\x01\x00"
PROFILE_FLAT = 1


def fnv1a_64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    for b in data:
        h ^= b
        h = (h * prime) & 0xFFFFFFFFFFFFFFFF
    return h


def hash_input(archive: bytes, index_size: int) -> bytes:
    archive_size = len(archive)
    suffix_start = archive_size - HASH_WINDOW
    index_end = INDEX_OFFSET + index_size
    if index_end <= suffix_start:
        return archive[INDEX_OFFSET:index_end] + archive[suffix_start:archive_size]
    return archive[INDEX_OFFSET:archive_size]


def parse_index(payload: bytes) -> dict[str, tuple[int, int]]:
    assert payload[:4] == INDEX_MAGIC, f"bad magic: {payload[:4]!r}"
    n = struct.unpack_from("<I", payload, 7)[0]
    cur = 11
    name_lens = struct.unpack_from(f"<{n}H", payload, cur)
    cur += 2 * n
    names = []
    for nl in name_lens:
        names.append(payload[cur : cur + nl].decode("ascii"))
        cur += nl
    offsets = struct.unpack_from(f"<{n}Q", payload, cur)
    cur += 8 * n
    sizes = struct.unpack_from(f"<{n}Q", payload, cur)
    return {nm: (off, sz) for nm, off, sz in zip(names, offsets, sizes)}


def index_size_from_lfh(archive: bytes) -> int:
    return struct.unpack("<I", archive[18:22])[0]


def extract_metadata_payload(archive: bytes) -> bytes:
    payload = archive[INDEX_OFFSET : INDEX_OFFSET + index_size_from_lfh(archive)]
    offset, size = parse_index(payload)["__metadata__"]
    return archive[offset : offset + size]


def assert_valid_cozip(data: bytes) -> None:
    assert len(data) >= HASH_WINDOW + INDEX_OFFSET
    assert data[:4] == ZIP_LFH_SIG
    assert data[30:39] == INDEX_NAME
    assert data[51:55] == INDEX_MAGIC
    expected = fnv1a_64(hash_input(data, index_size_from_lfh(data)))
    stored = struct.unpack("<Q", data[43:51])[0]
    assert stored == expected


@pytest.fixture
def fixtures(tmp_path: Path) -> dict[str, Path]:
    small = tmp_path / "small.txt"
    small.write_bytes(SMALL_CONTENT)
    medium = tmp_path / "medium.bin"
    medium.write_bytes(MEDIUM_CONTENT)
    return {"small": small, "medium": medium}


@pytest.fixture
def input_table(fixtures: dict[str, Path]) -> pa.Table:
    return pa.table(
        {
            "name": ["a.txt", "b.bin"],
            "path": [str(fixtures["small"]), str(fixtures["medium"])],
            "category": ["text", "binary"],
        }
    )


@pytest.fixture
def paths_arg(input_table: pa.Table) -> list[tuple[str, str]]:
    return list(
        zip(
            input_table.column("name").to_pylist(),
            input_table.column("path").to_pylist(),
        )
    )


@pytest.fixture
def archive(tmp_path: Path, input_table: pa.Table) -> Path:
    out = tmp_path / "out.zip"
    cozip.create(out, input_table)
    return out


class TestSpecInvariants:
    def test_archive_size_minimum(self, archive: Path) -> None:
        assert archive.stat().st_size >= HASH_WINDOW + INDEX_OFFSET

    def test_lfh_signature(self, archive: Path) -> None:
        assert archive.read_bytes()[:4] == ZIP_LFH_SIG

    def test_index_entry_filename(self, archive: Path) -> None:
        assert archive.read_bytes()[30:39] == INDEX_NAME

    def test_hash_block_header(self, archive: Path) -> None:
        assert archive.read_bytes()[39:43] == HASH_BLOCK_HEADER

    def test_index_header(self, archive: Path) -> None:
        data = archive.read_bytes()
        assert data[51:55] == INDEX_MAGIC
        assert data[55:57] == INDEX_VERSION
        assert data[57] == PROFILE_FLAT

    def test_eocd_comment_is_empty(self, archive: Path) -> None:
        assert archive.read_bytes()[-2:] == b"\x00\x00"

    def test_integrity_hash(self, archive: Path) -> None:
        data = archive.read_bytes()
        expected = fnv1a_64(hash_input(data, index_size_from_lfh(data)))
        stored = struct.unpack("<Q", data[43:51])[0]
        assert stored == expected, f"stored=0x{stored:016x} expected=0x{expected:016x}"

    def test_index_lists_only_metadata(self, archive: Path) -> None:
        data = archive.read_bytes()
        entries = parse_index(
            data[INDEX_OFFSET : INDEX_OFFSET + index_size_from_lfh(data)]
        )
        assert set(entries) == {"__metadata__"}
        offset, size = entries["__metadata__"]
        assert 0 < offset < len(data)
        assert 0 < size <= len(data) - offset

    def test_rejects_zero_byte_user_file(self, tmp_path: Path) -> None:
        empty = tmp_path / "empty.bin"
        empty.touch()
        table = pa.table({"name": ["empty.bin"], "path": [str(empty)]})
        output = tmp_path / "empty.zip"

        with pytest.raises(cozip.CozipError, match="zero-byte payload"):
            cozip.write(output, table)
        assert not output.exists()

    def test_rejects_directory_source(self, tmp_path: Path) -> None:
        table = pa.table({"name": ["directory"], "path": [str(tmp_path)]})

        with pytest.raises(ValueError, match="not a regular file"):
            cozip.stage_metadata(table)

    def test_output_cannot_replace_a_source(self, fixtures: dict[str, Path]) -> None:
        source = fixtures["small"]
        original = source.read_bytes()
        table = pa.table({"name": ["small.txt"], "path": [str(source)]})

        with pytest.raises(cozip.CozipError, match="also the source path"):
            cozip.write(source, table)
        assert source.read_bytes() == original

    def test_rejects_non_ascii_archive_name(self, fixtures: dict[str, Path]) -> None:
        table = pa.table({"name": ["niño.bin"], "path": [str(fixtures["small"])]})
        with pytest.raises(cozip.CozipError, match="non-ASCII"):
            cozip.stage_metadata(table)

    def test_rejects_nul_in_archive_name(self, fixtures: dict[str, Path]) -> None:
        table = pa.table({"name": ["bad\0name.bin"], "path": [str(fixtures["small"])]})
        with pytest.raises(ValueError, match="NUL byte"):
            cozip.stage_metadata(table)

    @pytest.mark.parametrize(
        "name",
        [
            "/absolute.bin",
            "C:/drive.bin",
            "folder/../escape.bin",
            "folder/./file.bin",
            "folder/",
            r"folder\file.bin",
        ],
    )
    def test_rejects_unsafe_archive_name(
        self, fixtures: dict[str, Path], name: str
    ) -> None:
        table = pa.table({"name": [name], "path": [str(fixtures["small"])]})

        with pytest.raises(cozip.CozipError, match="archive name"):
            cozip.stage_metadata(table)

    def test_accepts_non_ascii_source_path(self, tmp_path: Path) -> None:
        source = tmp_path / "niño.bin"
        source.write_bytes(b"x")
        table = pa.table({"name": ["data.bin"], "path": [str(source)]})

        staged, _ = cozip.stage_metadata(table)
        assert staged.column("size").to_pylist() == [1]

    def test_accepts_non_ascii_output_path(
        self, tmp_path: Path, input_table: pa.Table
    ) -> None:
        output = tmp_path / "niño.zip"
        cozip.write(output, input_table)
        assert output.read_bytes()[:4] == b"PK\x03\x04"

    def test_reader_rejects_non_ascii_source_before_loading_duckdb(
        self, tmp_path: Path
    ) -> None:
        with pytest.raises(ValueError, match="ASCII paths and URLs only"):
            cozip.read(tmp_path / "niño.zip")

    def test_reader_rejects_empty_source_before_loading_duckdb(self) -> None:
        with pytest.raises(ValueError, match="non-empty ASCII"):
            cozip.read("")

    def test_reader_rejects_nul_before_loading_duckdb(self) -> None:
        with pytest.raises(ValueError, match="NUL byte"):
            cozip.read("bad\0.zip")

    @pytest.mark.parametrize(
        ("kwargs", "message"),
        [
            ({"columns": "category"}, "sequence of non-empty strings"),
            ({"columns": [""]}, "sequence of non-empty strings"),
            ({"location": "yes"}, "must be a boolean"),
        ],
    )
    def test_reader_validates_options_before_loading_duckdb(
        self, kwargs, message
    ) -> None:
        with pytest.raises(TypeError, match=message):
            cozip.read("https://example.test/data.zip", **kwargs)

    def test_reader_projects_requested_columns_and_closes_connection(
        self, monkeypatch
    ) -> None:
        queries = []

        class FakeConnection:
            closed = False

            def execute(self, sql, params=None):
                queries.append((sql, params))
                return self

            def df(self):
                return "manifest"

            def close(self):
                self.closed = True

        con = FakeConnection()
        monkeypatch.setitem(sys.modules, "duckdb", SimpleNamespace(connect=lambda: con))

        result = cozip.read(
            "https://example.test/data.zip",
            columns=["category", 'sensor"name', "category"],
            location=False,
        )

        assert result == "manifest"
        assert queries[-1] == (
            (
                'SELECT "name", "offset", "size", "category", '
                '"sensor""name" FROM read_flat(?, location := false)'
            ),
            ["https://example.test/data.zip"],
        )
        assert con.closed

    def test_reader_drops_vsi_column_without_an_explicit_projection(
        self, monkeypatch
    ) -> None:
        queries = []

        class FakeConnection:
            def execute(self, sql, params=None):
                queries.append(sql)
                return self

            def df(self):
                return pd.DataFrame({"name": ["a"], "cozip:location": [None]})

            def close(self):
                pass

        monkeypatch.setitem(
            sys.modules, "duckdb", SimpleNamespace(connect=FakeConnection)
        )
        result = cozip.read("local.zip", location=False)

        assert queries[-1] == "SELECT * FROM read_flat(?, location := false)"
        assert list(result.columns) == ["name"]

    def test_reader_can_load_a_local_extension(self, monkeypatch) -> None:
        loaded = []
        connections = []

        class FakeConnection:
            def load_extension(self, path):
                loaded.append(path)

            def execute(self, sql, params=None):
                return self

            def df(self):
                return "manifest"

            def close(self):
                pass

        def connect(**options):
            connections.append(options)
            return FakeConnection()

        monkeypatch.setenv("COZIP_EXTENSION", "/tmp/cozip.duckdb_extension")
        monkeypatch.setitem(sys.modules, "duckdb", SimpleNamespace(connect=connect))

        assert cozip.read("local.zip") == "manifest"
        assert connections == [{"config": {"allow_unsigned_extensions": True}}]
        assert loaded == ["/tmp/cozip.duckdb_extension"]

    def test_reader_rejects_an_extension_without_read_flat(self, monkeypatch) -> None:
        class FakeConnection:
            def execute(self, sql, params=None):
                if "read_flat(" in sql:
                    raise RuntimeError("Table Function read_flat does not exist")
                return self

            def df(self):
                return "manifest"

            def close(self):
                pass

        monkeypatch.setitem(
            sys.modules, "duckdb", SimpleNamespace(connect=FakeConnection)
        )

        with pytest.raises(RuntimeError, match="reinstall it with INSTALL cozip"):
            cozip.read("local.zip")

    def test_parquet_suffix_still_uses_cozip_reader(self, monkeypatch) -> None:
        queries = []

        class FakeConnection:
            def execute(self, sql, params=None):
                queries.append((sql, params))
                return self

            def df(self):
                return "manifest"

            def close(self):
                pass

        monkeypatch.setitem(
            sys.modules, "duckdb", SimpleNamespace(connect=FakeConnection)
        )

        assert cozip.read("not-a-cozip.parquet") == "manifest"
        assert queries[-1] == (
            "SELECT * FROM read_flat(?, location := true)",
            ["not-a-cozip.parquet"],
        )


class TestZipCompatibility:
    def test_stdlib_zipfile_lists_all_entries(self, archive: Path) -> None:
        with zipfile.ZipFile(archive) as zf:
            names = set(zf.namelist())
        assert {"__cozip__", "__metadata__", "a.txt", "b.bin"} <= names

    def test_all_entries_use_store(self, archive: Path) -> None:
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                assert info.compress_type == zipfile.ZIP_STORED, info.filename

    def test_no_encryption_no_data_descriptor(self, archive: Path) -> None:
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                assert info.flag_bits & 0x01 == 0, f"{info.filename} encrypted"
                assert info.flag_bits & 0x08 == 0, f"{info.filename} has DD"

    def test_user_payloads_byte_exact(self, archive: Path) -> None:
        with zipfile.ZipFile(archive) as zf:
            assert zf.read("a.txt") == SMALL_CONTENT
            assert zf.read("b.bin") == MEDIUM_CONTENT


class TestStageMetadata:
    def test_drops_path_column(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        assert "path" not in meta.column_names

    def test_adds_offset_and_size_as_uint64(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        assert meta.schema.field("offset").type == pa.uint64()
        assert meta.schema.field("size").type == pa.uint64()

    def test_preserves_user_extras(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        assert meta.column("category").to_pylist() == ["text", "binary"]

    def test_canonical_column_order(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        assert meta.column_names[:3] == ["name", "offset", "size"]

    def test_sizes_match_source_lengths(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        assert meta.column("size").to_pylist() == [
            len(SMALL_CONTENT),
            len(MEDIUM_CONTENT),
        ]

    def test_offsets_strictly_increasing(self, input_table: pa.Table) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        offsets = meta.column("offset").to_pylist()
        assert all(a < b for a, b in pairwise(offsets))

    def test_returns_paths_aligned_with_table(self, input_table: pa.Table) -> None:
        meta, paths = cozip.stage_metadata(input_table)
        expected = list(
            zip(
                input_table.column("name").to_pylist(),
                input_table.column("path").to_pylist(),
            )
        )
        assert paths == expected
        assert len(paths) == meta.num_rows

    def test_paths_drive_stage_create(
        self, tmp_path: Path, input_table: pa.Table
    ) -> None:
        meta, paths = cozip.stage_metadata(input_table)
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(meta, meta_pq)
        out = tmp_path / "out.zip"
        cozip.stage_create(out, paths, meta_pq)
        assert_valid_cozip(out.read_bytes())

    def test_rejects_offset_in_input(self, fixtures: dict[str, Path]) -> None:
        bad = pa.table(
            {
                "name": ["a.txt"],
                "path": [str(fixtures["small"])],
                "offset": [0],
            }
        )
        with pytest.raises(ValueError, match="reserved"):
            cozip.stage_metadata(bad)

    def test_rejects_duplicate_names(self, fixtures: dict[str, Path]) -> None:
        bad = pa.table(
            {
                "name": ["x", "x"],
                "path": [str(fixtures["small"]), str(fixtures["medium"])],
            }
        )
        with pytest.raises(ValueError, match="duplicate"):
            cozip.stage_metadata(bad)

    def test_rejects_reserved_archive_name(self, fixtures: dict[str, Path]) -> None:
        bad = pa.table(
            {
                "name": ["__metadata__"],
                "path": [str(fixtures["small"])],
            }
        )
        with pytest.raises(ValueError, match="reserved"):
            cozip.stage_metadata(bad)


class TestStageCreate:
    def _write_meta(self, tmp_path: Path, input_table: pa.Table) -> Path:
        meta, _ = cozip.stage_metadata(input_table)
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(meta, meta_pq)
        return meta_pq

    def test_validate_must_be_boolean(self, tmp_path: Path) -> None:
        with pytest.raises(TypeError, match="validate must be a boolean"):
            cozip.stage_create(
                tmp_path / "out.zip", [], tmp_path / "metadata.parquet", "yes"
            )

    def test_rejects_malformed_path_pairs(
        self, tmp_path: Path, input_table: pa.Table
    ) -> None:
        meta_pq = self._write_meta(tmp_path, input_table)
        with pytest.raises(ValueError, match=r"paths\[0\].*pair"):
            cozip.stage_create(tmp_path / "out.zip", ["ab"], meta_pq)

    def test_packs_a_valid_archive(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        out = tmp_path / "out.zip"
        meta_pq = self._write_meta(tmp_path, input_table)
        cozip.stage_create(out, paths_arg, meta_pq)
        assert_valid_cozip(out.read_bytes())

    def test_metadata_parquet_embedded_verbatim(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        meta_pq = self._write_meta(tmp_path, input_table)
        original_bytes = meta_pq.read_bytes()

        out = tmp_path / "out.zip"
        cozip.stage_create(out, paths_arg, meta_pq)

        embedded = extract_metadata_payload(out.read_bytes())
        assert embedded == original_bytes

    def test_rejects_parquet_with_path_column(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        tampered = meta.append_column("path", pa.array(["/tmp/a", "/tmp/b"]))
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(tampered, meta_pq)

        with pytest.raises(ValueError, match="path"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)

    def test_rejects_parquet_missing_required_columns(
        self, tmp_path: Path, paths_arg
    ) -> None:
        bad = pa.table({"name": ["a.txt", "b.bin"]})
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(bad, meta_pq)

        with pytest.raises(ValueError, match="required column"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)

    def test_rejects_empty_metadata_file(self, tmp_path: Path, paths_arg) -> None:
        meta_pq = tmp_path / "empty.parquet"
        meta_pq.touch()

        with pytest.raises(ValueError, match="metadata parquet is empty"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)

    def test_rejects_null_metadata_values(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        index = meta.column_names.index("name")
        bad = meta.set_column(index, "name", pa.array(["a.txt", None]))
        meta_pq = tmp_path / "null.parquet"
        pq.write_table(bad, meta_pq)

        with pytest.raises(ValueError, match="NULL name at row 1"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)

    def test_rejects_offset_mismatch(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        idx = meta.column_names.index("offset")
        bad = meta.set_column(idx, "offset", pa.array([0, 0], type=pa.uint64()))
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(bad, meta_pq)

        with pytest.raises(ValueError, match="offset mismatch"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)

    def test_rejects_row_count_mismatch(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        meta, _ = cozip.stage_metadata(input_table)
        truncated = meta.slice(0, 1)
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(truncated, meta_pq)

        with pytest.raises(ValueError, match="rows"):
            cozip.stage_create(tmp_path / "out.zip", paths_arg, meta_pq)


class TestSchemaMetadataRoundTrip:
    def test_arrow_schema_metadata_is_preserved(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        geo_value = (
            b'{"version":"1.0.0","primary_column":"geometry",'
            b'"columns":{"geometry":{"encoding":"WKB","crs":null}}}'
        )
        meta, _ = cozip.stage_metadata(input_table)
        tagged = meta.replace_schema_metadata(
            {b"geo": geo_value, b"asterisk:tag": b"cozip-conformance"}
        )
        meta_pq = tmp_path / "meta.parquet"
        pq.write_table(tagged, meta_pq)

        out = tmp_path / "out.zip"
        cozip.stage_create(out, paths_arg, meta_pq)

        embedded = extract_metadata_payload(out.read_bytes())
        recovered = pq.read_table(io.BytesIO(embedded))

        assert recovered.schema.metadata is not None
        assert recovered.schema.metadata.get(b"geo") == geo_value
        assert recovered.schema.metadata.get(b"asterisk:tag") == b"cozip-conformance"


class TestGeopandasRoundTrip:
    def test_geopandas_reads_embedded_geoparquet(
        self, tmp_path: Path, input_table: pa.Table, paths_arg
    ) -> None:
        gpd = pytest.importorskip("geopandas")
        from shapely.geometry import Point

        meta, _ = cozip.stage_metadata(input_table)
        df = meta.to_pandas()
        gdf = gpd.GeoDataFrame(
            df,
            geometry=[Point(-77.04, -12.05), Point(2.35, 48.86)],
            crs="EPSG:4326",
        )
        meta_pq = tmp_path / "meta.parquet"
        gdf.to_parquet(meta_pq)

        out = tmp_path / "out.zip"
        cozip.stage_create(out, paths_arg, meta_pq)

        extracted = tmp_path / "extracted.parquet"
        extracted.write_bytes(extract_metadata_payload(out.read_bytes()))
        recovered = gpd.read_parquet(extracted)

        assert isinstance(recovered, gpd.GeoDataFrame)
        assert recovered.crs.to_string() == "EPSG:4326"
        assert recovered["name"].tolist() == ["a.txt", "b.bin"]
        assert recovered["offset"].tolist() == df["offset"].tolist()
        assert recovered["size"].tolist() == df["size"].tolist()

        first, second = recovered.geometry.iloc[0], recovered.geometry.iloc[1]
        assert (first.x, first.y) == (-77.04, -12.05)
        assert (second.x, second.y) == (2.35, 48.86)
