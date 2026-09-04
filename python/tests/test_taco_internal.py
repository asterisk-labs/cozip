"""Conformance tests for cozip's private, payload-opaque TACO writer."""

from __future__ import annotations

import struct
import subprocess
import sys
import zipfile
from dataclasses import replace
from pathlib import Path

import cozip
import pytest
from cozip._core import (
    COZIP_ERR_INVALID_ARGUMENT,
    COZIP_ERR_INVALID_LFH,
    COZIP_ERR_IO,
    COZIP_SOURCE_BUFFER,
    CozipError,
    ffi,
    lib,
)
from cozip._taco import API_VERSION, Plan, plan, write

INDEX_OFFSET = 51
HASH_WINDOW = 32_768
COLLECTION = "COLLECTION.json"


def _fnv1a_64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def _parse_index(archive: bytes) -> tuple[int, dict[str, tuple[int, int]]]:
    index_size = struct.unpack_from("<I", archive, 18)[0]
    payload = archive[INDEX_OFFSET : INDEX_OFFSET + index_size]
    assert payload[:4] == b"CZIP"
    profile = payload[6]
    count = struct.unpack_from("<I", payload, 7)[0]
    cursor = 11
    lengths = struct.unpack_from(f"<{count}H", payload, cursor)
    cursor += 2 * count
    names = []
    for length in lengths:
        names.append(payload[cursor : cursor + length].decode("ascii"))
        cursor += length
    offsets = struct.unpack_from(f"<{count}Q", payload, cursor)
    cursor += 8 * count
    sizes = struct.unpack_from(f"<{count}Q", payload, cursor)
    return profile, {
        name: (offset, size) for name, offset, size in zip(names, offsets, sizes)
    }


@pytest.fixture
def physical_files(tmp_path: Path) -> dict[str, Path]:
    image = tmp_path / "image.tif"
    mask = tmp_path / "mask.tif"
    collection = tmp_path / "COLLECTION.json"
    metadata = tmp_path / "sample.parquet"
    image.write_bytes(b"opaque-image-bytes" * 7)
    mask.write_bytes(b"opaque-mask-bytes" * 11)
    # Deliberately invalid JSON and Parquet: cozip must never inspect them.
    collection.write_bytes(b"opaque collection payload, owned by TACO")
    metadata.write_bytes(b"opaque metadata payload, owned by TACO")
    return {
        "image": image,
        "mask": mask,
        "collection": collection,
        "metadata": metadata,
    }


@pytest.fixture
def layout(physical_files: dict[str, Path]) -> Plan:
    return plan(
        files=[
            ("DATA/0/image.tif", physical_files["image"]),
            ("DATA/0/mask.tif", physical_files["mask"]),
        ],
        priority_names=["COLLECTION.json", "METADATA/sample.parquet"],
    )


def _write_fixture(output: Path, layout: Plan, physical_files: dict[str, Path]) -> Path:
    return Path(
        write(
            output,
            layout,
            priority_files=[
                ("COLLECTION.json", physical_files["collection"]),
                ("METADATA/sample.parquet", physical_files["metadata"]),
            ],
        )
    )


@pytest.fixture
def taco_archive(tmp_path: Path, physical_files: dict[str, Path], layout: Plan) -> Path:
    return _write_fixture(tmp_path / "dataset.zip", layout, physical_files)


class TestPrivateBoundary:
    def test_private_api_version_is_explicit(self) -> None:
        assert API_VERSION == 1

    def test_public_cozip_api_is_unchanged(self) -> None:
        assert cozip.write is cozip.create
        assert "plan_taco" not in cozip.__all__
        assert "write_taco" not in cozip.__all__
        assert not hasattr(cozip, "plan_taco")
        assert not hasattr(cozip, "write_taco")

    def test_private_module_has_a_lightweight_import(self) -> None:
        code = """
import sys
import cozip._taco
assert 'pyarrow' not in sys.modules
assert 'pyarrow.parquet' not in sys.modules
assert 'pandas' not in sys.modules
assert 'duckdb' not in sys.modules
"""
        subprocess.run([sys.executable, "-c", code], check=True)


class TestPlan:
    def test_c_returns_sizes_offsets_and_priority_identity(
        self, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        assert layout.priority_names == (
            "COLLECTION.json",
            "METADATA/sample.parquet",
        )
        assert [item.size for item in layout.files] == [
            physical_files["image"].stat().st_size,
            physical_files["mask"].stat().st_size,
        ]
        assert layout.files[0].offset < layout.files[1].offset
        assert layout.offsets == {
            item.name: (item.offset, item.size) for item in layout.files
        }

    def test_priority_name_lengths_change_data_offsets(
        self, physical_files: dict[str, Path]
    ) -> None:
        files = [("DATA/0/image.tif", physical_files["image"])]
        short = plan(files, [COLLECTION, "p"])
        long = plan(files, [COLLECTION, "priority-name-that-is-longer"])
        assert long.files[0].offset > short.files[0].offset

    def test_priority_payload_does_not_need_to_exist_during_plan(
        self, physical_files: dict[str, Path]
    ) -> None:
        result = plan(
            [("DATA/0/image.tif", physical_files["image"])],
            [COLLECTION, "METADATA/future.parquet"],
        )
        assert result.priority_names == (COLLECTION, "METADATA/future.parquet")

    def test_allows_no_non_priority_files(self) -> None:
        result = plan([], [COLLECTION])
        assert result.files == ()
        assert result.offsets == {}

    def test_rejects_missing_collection_json(
        self, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError, match="COLLECTION.json") as captured:
            plan(
                [("DATA/0/image.tif", physical_files["image"])],
                ["METADATA/sample.parquet"],
            )
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    def test_rejects_metadata_parquet_outside_the_index(
        self, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError, match="14.2") as captured:
            plan([("METADATA/stray.parquet", physical_files["image"])], [COLLECTION])
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    def test_rejects_dot_parquet_outside_the_index(
        self, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError, match="14.2"):
            plan([("METADATA/.parquet", physical_files["image"])], [COLLECTION])

    def test_non_parquet_metadata_and_data_parquet_may_stay_non_priority(
        self, physical_files: dict[str, Path]
    ) -> None:
        result = plan(
            [
                ("METADATA/notes.txt", physical_files["image"]),
                ("DATA/0/table.parquet", physical_files["mask"]),
            ],
            [COLLECTION],
        )
        assert set(result.offsets) == {"METADATA/notes.txt", "DATA/0/table.parquet"}

    def test_rejects_directory_source(self, tmp_path: Path) -> None:
        with pytest.raises(CozipError, match="regular file") as captured:
            plan([("DATA/0/dir", tmp_path)], [COLLECTION])
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    def test_accepts_non_ascii_source_path(self, tmp_path: Path) -> None:
        source = tmp_path / "niño.bin"
        source.write_bytes(b"x")
        result = plan([("DATA/0/file.bin", source)], [COLLECTION])
        assert result.files[0].size == 1

    def test_rejects_nul_in_name(self, physical_files: dict[str, Path]) -> None:
        with pytest.raises(ValueError, match="NUL"):
            plan([("DATA/0/bad\0name", physical_files["image"])], [COLLECTION])

    def test_rejects_no_priorities(self, physical_files: dict[str, Path]) -> None:
        with pytest.raises(CozipError, match="at least one priority") as captured:
            plan([("data", physical_files["image"])], [])
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    def test_rejects_missing_source(self, tmp_path: Path) -> None:
        with pytest.raises(CozipError) as captured:
            plan([("data", tmp_path / "missing")], [COLLECTION])
        assert captured.value.code == COZIP_ERR_IO

    def test_rejects_zero_size_source(self, tmp_path: Path) -> None:
        empty = tmp_path / "empty"
        empty.touch()
        with pytest.raises(CozipError, match="empty") as captured:
            plan([("data", empty)], [COLLECTION])
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    @pytest.mark.parametrize(
        "name",
        [
            "",
            "/absolute",
            "C:/drive",
            "folder/../escape",
            "folder/./file",
            "folder/",
            r"folder\file",
            "__cozip__",
            "__cozip_padding__",
        ],
    )
    def test_rejects_invalid_physical_names(
        self, name: str, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError) as captured:
            plan([(name, physical_files["image"])], [COLLECTION])
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT

    def test_rejects_duplicate_names_across_partitions(
        self, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError, match="duplicate"):
            plan([("same", physical_files["image"])], [COLLECTION, "same"])

    def test_rejects_name_over_zip_limit(self, physical_files: dict[str, Path]) -> None:
        with pytest.raises(CozipError, match="length"):
            plan([("x" * 65_536, physical_files["image"])], [COLLECTION])


class TestWrite:
    def test_profile_and_priority_index(self, taco_archive: Path) -> None:
        profile, index = _parse_index(taco_archive.read_bytes())
        assert profile == 2
        assert list(index) == ["COLLECTION.json", "METADATA/sample.parquet"]

    def test_all_payloads_are_byte_exact_and_opaque(
        self, taco_archive: Path, physical_files: dict[str, Path]
    ) -> None:
        with zipfile.ZipFile(taco_archive) as archive:
            assert (
                archive.read("DATA/0/image.tif") == physical_files["image"].read_bytes()
            )
            assert (
                archive.read("DATA/0/mask.tif") == physical_files["mask"].read_bytes()
            )
            assert (
                archive.read("COLLECTION.json")
                == physical_files["collection"].read_bytes()
            )
            assert (
                archive.read("METADATA/sample.parquet")
                == physical_files["metadata"].read_bytes()
            )

    def test_planned_offsets_resolve_exact_data_payloads(
        self, taco_archive: Path, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        raw = taco_archive.read_bytes()
        for item in layout.files:
            assert (
                raw[item.offset : item.offset + item.size] == item.source.read_bytes()
            )

    def test_small_archive_padding_precedes_priority_block(
        self, taco_archive: Path
    ) -> None:
        with zipfile.ZipFile(taco_archive) as archive:
            assert archive.namelist() == [
                "__cozip__",
                "DATA/0/image.tif",
                "DATA/0/mask.tif",
                "__cozip_padding__",
                "COLLECTION.json",
                "METADATA/sample.parquet",
            ]

    def test_large_archive_has_no_padding_and_priorities_stay_last(
        self, tmp_path: Path, physical_files: dict[str, Path]
    ) -> None:
        physical_files["image"].write_bytes(b"x" * 40_000)
        large_layout = plan(
            [("DATA/0/image.tif", physical_files["image"])],
            ["COLLECTION.json", "METADATA/sample.parquet"],
        )
        output = _write_fixture(tmp_path / "large.zip", large_layout, physical_files)
        with zipfile.ZipFile(output) as archive:
            names = archive.namelist()
        assert "__cozip_padding__" not in names
        assert names[-2:] == ["COLLECTION.json", "METADATA/sample.parquet"]

    def test_every_zip_entry_uses_store(self, taco_archive: Path) -> None:
        with zipfile.ZipFile(taco_archive) as archive:
            assert all(
                item.compress_type == zipfile.ZIP_STORED for item in archive.infolist()
            )

    def test_integrity_hash_covers_final_layout(self, taco_archive: Path) -> None:
        raw = taco_archive.read_bytes()
        index_size = struct.unpack_from("<I", raw, 18)[0]
        index_end = INDEX_OFFSET + index_size
        suffix_start = len(raw) - HASH_WINDOW
        hash_bytes = raw[INDEX_OFFSET:index_end]
        if index_end <= suffix_start:
            hash_bytes += raw[suffix_start:]
        else:
            hash_bytes += raw[index_end:]
        assert struct.unpack_from("<Q", raw, 43)[0] == _fnv1a_64(hash_bytes)

    def test_rejects_non_ascii_archive_names(
        self, tmp_path: Path, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError, match="non-ASCII"):
            plan(
                [("DATA/0/niño.tif", physical_files["image"])],
                [COLLECTION],
            )

    def test_accepts_non_ascii_output_path(
        self, tmp_path: Path, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        output = tmp_path / "niño.zip"
        _write_fixture(output, layout, physical_files)
        assert output.read_bytes()[:4] == b"PK\x03\x04"

    def test_can_write_priority_only_archive(
        self, tmp_path: Path, physical_files: dict[str, Path]
    ) -> None:
        empty_layout = plan([], [COLLECTION])
        output = Path(
            write(
                tmp_path / "priority-only.zip",
                empty_layout,
                [(COLLECTION, physical_files["collection"])],
            )
        )
        _, index = _parse_index(output.read_bytes())
        assert list(index) == [COLLECTION]

    def test_archive_ends_with_comment_free_eocd(self, taco_archive: Path) -> None:
        raw = taco_archive.read_bytes()
        assert raw[-22:-18] == b"PK\x05\x06"
        assert raw[-2:] == b"\x00\x00"

    def test_priority_block_is_one_contiguous_final_region(
        self, taco_archive: Path
    ) -> None:
        raw = taco_archive.read_bytes()
        _, index = _parse_index(raw)
        start = min(offset for offset, _ in index.values())
        end = max(offset + size for offset, size in index.values())
        with zipfile.ZipFile(taco_archive) as archive:
            infos = archive.infolist()
        # No non-priority local header may sit inside the priority region,
        # and nothing may follow it before the Central Directory.
        for info in infos:
            if info.filename not in index:
                assert info.header_offset < start
        assert end <= struct.unpack_from("<I", raw, len(raw) - 22 + 16)[0]

    def test_existing_output_is_replaced_like_flat(
        self,
        tmp_path: Path,
        layout: Plan,
        physical_files: dict[str, Path],
    ) -> None:
        output = tmp_path / "existing.zip"
        output.write_bytes(b"old")
        _write_fixture(output, layout, physical_files)
        assert output.read_bytes()[:4] == b"PK\x03\x04"

    def test_output_cannot_overwrite_a_source(
        self, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        before = physical_files["image"].read_bytes()
        with pytest.raises(CozipError, match="also source path"):
            _write_fixture(physical_files["image"], layout, physical_files)
        assert physical_files["image"].read_bytes() == before


class TestPlanWriteConsistency:
    def test_rejects_priority_name_or_order_drift(
        self,
        tmp_path: Path,
        layout: Plan,
        physical_files: dict[str, Path],
    ) -> None:
        with pytest.raises(ValueError, match="differ from plan"):
            write(
                tmp_path / "wrong-priority.zip",
                layout,
                [("OTHER", physical_files["metadata"])],
            )

    def test_c_rejects_data_size_drift_before_touching_output(
        self,
        tmp_path: Path,
        layout: Plan,
        physical_files: dict[str, Path],
    ) -> None:
        output = tmp_path / "changed.zip"
        physical_files["image"].write_bytes(b"changed-size")
        with pytest.raises(CozipError, match="differ from plan") as captured:
            _write_fixture(output, layout, physical_files)
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT
        assert not output.exists()

    def test_c_rejects_tampered_planned_offset(
        self,
        tmp_path: Path,
        layout: Plan,
        physical_files: dict[str, Path],
    ) -> None:
        first = replace(layout.files[0], offset=layout.files[0].offset + 1)
        tampered = replace(layout, files=(first, *layout.files[1:]))
        with pytest.raises(CozipError, match="layout differs"):
            _write_fixture(tmp_path / "tampered.zip", tampered, physical_files)

    def test_c_rejects_tampered_plan_token(
        self,
        tmp_path: Path,
        layout: Plan,
        physical_files: dict[str, Path],
    ) -> None:
        token = replace(layout._native, layout_hash=layout._native.layout_hash ^ 1)
        tampered = replace(layout, _native=token)
        with pytest.raises(CozipError, match="differ from plan"):
            _write_fixture(tmp_path / "token.zip", tampered, physical_files)

    def test_c_rejects_missing_priority_payload(
        self, tmp_path: Path, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        with pytest.raises(CozipError) as captured:
            write(
                tmp_path / "missing-priority.zip",
                layout,
                [
                    ("COLLECTION.json", tmp_path / "missing.json"),
                    ("METADATA/sample.parquet", physical_files["metadata"]),
                ],
            )
        assert captured.value.code == COZIP_ERR_IO

    def test_c_rejects_empty_priority_payload(
        self, tmp_path: Path, layout: Plan, physical_files: dict[str, Path]
    ) -> None:
        empty = tmp_path / "empty.parquet"
        empty.touch()
        with pytest.raises(CozipError, match="empty") as captured:
            write(
                tmp_path / "empty-priority.zip",
                layout,
                [
                    ("COLLECTION.json", physical_files["collection"]),
                    ("METADATA/sample.parquet", empty),
                ],
            )
        assert captured.value.code == COZIP_ERR_INVALID_ARGUMENT


class TestPostWriteVerification:
    def test_write_archive_rejects_unplanned_layout_and_removes_output(
        self, tmp_path: Path
    ) -> None:
        payload = b"unplanned-bytes"
        output = tmp_path / "unplanned.zip"

        entries = ffi.new("cozip_entry_t[1]")
        name = ffi.new("char[]", b"data.bin")
        buffer = ffi.new("uint8_t[]", payload)
        entries[0].arc_name = name
        entries[0].payload_size = len(payload)
        entries[0].in_index = False
        entries[0].source.kind = COZIP_SOURCE_BUFFER
        entries[0].source.u.buffer.data = buffer
        entries[0].source.u.buffer.size = len(payload)

        index_payload = ffi.new("uint8_t[11]")
        err = ffi.new("cozip_error_t *")
        assert lib.cozip_build_index_payload(entries, 1, 0, index_payload, 11, err) == 0

        # lfh_offset was never planned, so the on-disk layout cannot match.
        output_c = ffi.new("char[]", str(output).encode())
        status = lib.cozip_write_archive(output_c, entries, 1, index_payload, 11, err)
        assert status == COZIP_ERR_INVALID_LFH
        assert not output.exists()

        assert lib.cozip_plan(entries, 1, err) == 0
        assert (
            lib.cozip_write_archive(output_c, entries, 1, index_payload, 11, err) == 0
        )
        assert output.exists()

    def test_write_archive_rejects_zero_byte_payload(self, tmp_path: Path) -> None:
        output = tmp_path / "zero.zip"
        entries = ffi.new("cozip_entry_t[1]")
        name = ffi.new("char[]", b"empty.bin")
        placeholder = ffi.new("uint8_t[]", b"x")
        entries[0].arc_name = name
        entries[0].payload_size = 0
        entries[0].source.kind = COZIP_SOURCE_BUFFER
        entries[0].source.u.buffer.data = placeholder
        entries[0].source.u.buffer.size = 0

        index_payload = ffi.new("uint8_t[11]")
        err = ffi.new("cozip_error_t *")
        assert lib.cozip_plan(entries, 1, err) == 0
        assert lib.cozip_build_index_payload(entries, 1, 0, index_payload, 11, err) == 0
        output_c = ffi.new("char[]", str(output).encode())

        status = lib.cozip_write_archive(output_c, entries, 1, index_payload, 11, err)
        assert status == COZIP_ERR_INVALID_ARGUMENT
        assert "zero-byte payload" in ffi.string(err.message).decode()
        assert not output.exists()
