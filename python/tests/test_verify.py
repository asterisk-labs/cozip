"""cozip.verify: the index and integrity hash of an archive of any profile."""
import subprocess
import sys
from pathlib import Path

import pyarrow as pa
import pytest

import cozip
from cozip._core import (
    COZIP_ERR_ARCHIVE_TOO_SMALL,
    COZIP_ERR_DUPLICATE_NAME,
    COZIP_ERR_HASH_MISMATCH,
    COZIP_ERR_INVALID_LFH,
    COZIP_ERR_INVALID_NAME,
    COZIP_ERR_INVALID_ZIP_STRUCTURE,
)

INDEX_OFFSET = 51
WINDOW = 32768


def integrity_hash(data: bytes) -> int:
    """FNV-1a 64 over the index and the last 32 KiB, as spec 8.2 defines."""
    index_end = INDEX_OFFSET + int.from_bytes(data[18:22], "little")
    suffix_start = len(data) - WINDOW
    h = 0xCBF29CE484222325
    for byte in data[INDEX_OFFSET:index_end] + data[max(index_end, suffix_start):]:
        h = ((h ^ byte) * 0x100000001B3) & ((1 << 64) - 1)
    return h


def rehashed(data: bytearray) -> bytearray:
    data[43:51] = integrity_hash(bytes(data)).to_bytes(8, "little")
    return data


@pytest.fixture
def archive(tmp_path: Path) -> Path:
    source = tmp_path / "a.txt"
    source.write_bytes(b"hello cozip\n" * 8)
    output = tmp_path / "out.zip"
    cozip.write(output, pa.table({"name": ["a.txt"], "path": [str(source)]}))
    return output


def patched(archive: Path, patch, rehash: bool) -> Path:
    data = bytearray(archive.read_bytes())
    patch(data)
    if rehash:
        rehashed(data)
    out = archive.with_name("patched.zip")
    out.write_bytes(bytes(data))
    return out


class TestVerify:
    def test_intact_archive_returns_none(self, archive: Path) -> None:
        assert cozip.verify(archive) is None
        assert cozip.verify(str(archive)) is None

    def test_any_profile_value_is_accepted(self, archive: Path) -> None:
        # Spec 7.1: a reader may parse an index whose profile it does not know.
        for profile in (0, 2, 7):
            def patch(data: bytearray, profile: int = profile) -> None:
                data[57] = profile
            assert cozip.verify(patched(archive, patch, rehash=True)) is None

    def test_reads_over_http(self, archive: Path, range_server) -> None:
        assert cozip.verify(f"{range_server.url}/{archive.name}") is None
        # The head and the tail, one request each, and nothing else.
        assert len(range_server.requests) == 2

    @pytest.mark.parametrize(
        ("patch", "rehash", "code", "message"),
        [
            (lambda d: d.__setitem__(0, ord("X")), False, COZIP_ERR_INVALID_LFH,
             "byte 0 is not a ZIP Local File Header"),
            (lambda d: d.__setitem__(43, d[43] ^ 1), False, COZIP_ERR_HASH_MISMATCH,
             "cozip integrity hash mismatch"),
            (lambda d: d.__setitem__(len(d) - 30, d[len(d) - 30] ^ 1), False,
             COZIP_ERR_HASH_MISMATCH, "cozip integrity hash mismatch"),
            (lambda d: d.__setitem__(len(d) - 2, 1), True,
             COZIP_ERR_INVALID_ZIP_STRUCTURE, "comment-free End of Central Directory"),
            (lambda d: d.__setitem__(64, ord("/")), True, COZIP_ERR_INVALID_NAME,
             "entry 0 name must be relative"),
        ],
        ids=["header", "hash", "tail", "end record", "name"],
    )
    def test_names_the_broken_rule(self, archive, patch, rehash, code, message) -> None:
        bad = patched(archive, patch, rehash)
        with pytest.raises(cozip.CozipError, match=message) as excinfo:
            cozip.verify(bad)
        assert excinfo.value.code == code
        assert str(bad) in str(excinfo.value)

    def test_rejects_a_truncated_archive(self, archive: Path, tmp_path: Path) -> None:
        # A valid header passes step 1 of spec 8.5; the size fails step 5.
        small = tmp_path / "small.zip"
        small.write_bytes(archive.read_bytes()[: INDEX_OFFSET + WINDOW - 1])
        with pytest.raises(cozip.CozipError, match="too small") as excinfo:
            cozip.verify(small)
        assert excinfo.value.code == COZIP_ERR_ARCHIVE_TOO_SMALL

    def test_missing_file(self, tmp_path: Path) -> None:
        with pytest.raises(cozip.CozipError, match="missing.zip") as excinfo:
            cozip.verify(tmp_path / "missing.zip")
        assert excinfo.value.name in {"IO", "NOT_FOUND"}

    def test_hides_credentials(self, archive: Path, range_server) -> None:
        bad = patched(archive, lambda d: d.__setitem__(43, d[43] ^ 1), rehash=False)
        url = f"{range_server.url}/{bad.name}?X-Amz-Signature=TOPSECRET"
        with pytest.raises(cozip.CozipError) as excinfo:
            cozip.verify(url)
        assert excinfo.value.code == COZIP_ERR_HASH_MISMATCH
        assert "TOPSECRET" not in str(excinfo.value)

    @pytest.mark.parametrize(
        ("source", "error"),
        [
            (b"out.zip", TypeError),
            ("", ValueError),
            ("a\0.zip", ValueError),
            ("niño.zip", ValueError),
        ],
    )
    def test_checks_the_source_first(self, source, error) -> None:
        with pytest.raises(error, match="cozip.verify"):
            cozip.verify(source)

    def test_imports_without_pandas(self) -> None:
        code = (
            "import sys, cozip\n"
            "cozip.verify\n"
            "assert 'pandas' not in sys.modules\n"
            "assert 'duckdb' not in sys.modules\n"
        )
        subprocess.run([sys.executable, "-c", code], check=True)


class TestDuplicateNames:
    def test_duplicate_priority_names(self, tmp_path: Path) -> None:
        # Two priority files, then the second renamed onto the first.
        from cozip._core import COZIP_PROFILE_NONE, COZIP_SOURCE_BUFFER, ffi, lib

        payloads = [b"first", b"third"]
        names = [ffi.new("char[]", b"a.bin"), ffi.new("char[]", b"c.bin")]
        buffers = [ffi.new("uint8_t[]", p) for p in payloads]
        entries = ffi.new("cozip_entry_t[]", 3)
        for i, payload in enumerate(payloads):
            entries[i].arc_name = names[i]
            entries[i].payload_size = len(payload)
            entries[i].in_index = True
            entries[i].source.kind = COZIP_SOURCE_BUFFER
            entries[i].source.u.buffer.data = buffers[i]
            entries[i].source.u.buffer.size = len(payload)
        err = ffi.new("cozip_error_t *")
        output = tmp_path / "two.zip"
        assert lib.cozip_finalize(
            str(output).encode(), entries, 2, 3, COZIP_PROFILE_NONE, err
        ) == 0, ffi.string(err.message)
        assert cozip.verify(output) is None

        data = bytearray(output.read_bytes())
        # The index names start after the 11-byte header and two lengths.
        names_at = INDEX_OFFSET + 11 + 4
        assert bytes(data[names_at:names_at + 10]) == b"a.binc.bin"
        data[names_at + 5] = ord("a")
        bad = tmp_path / "dup.zip"
        bad.write_bytes(bytes(rehashed(data)))
        with pytest.raises(cozip.CozipError, match="share the name 'a.bin'") as excinfo:
            cozip.verify(bad)
        assert excinfo.value.code == COZIP_ERR_DUPLICATE_NAME
