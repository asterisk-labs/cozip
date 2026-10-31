"""Real reads of Flat archives through the native reader."""

import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

import cozip
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from cozip._core import (
    COZIP_ERR_INVALID_INDEX,
    COZIP_ERR_PROFILE,
    COZIP_SOURCE_PATH,
    ffi,
    lib,
)
from cozip._reader import _location_base
from cozip._taco import plan, write

SMALL_CONTENT = b"hello cozip\n" * 8
LARGE_CONTENT = bytes(range(256)) * 8192


@pytest.fixture
def sources(tmp_path: Path) -> dict[str, Path]:
    small = tmp_path / "small.txt"
    small.write_bytes(SMALL_CONTENT)
    large = tmp_path / "large.bin"
    large.write_bytes(LARGE_CONTENT)
    return {"a.txt": small, "dir/b.bin": large}


@pytest.fixture
def archive(tmp_path: Path, sources: dict[str, Path]) -> Path:
    output = tmp_path / "out.zip"
    cozip.create(
        output,
        pa.table(
            {
                "name": list(sources),
                "path": [str(path) for path in sources.values()],
                "category": ["text", "binary"],
                'sensor"name': ["s1", "s2"],
                "score": pa.array([1, None], pa.int32()),
            }
        ),
    )
    return output


def stored_manifest(archive: Path) -> pa.Table:
    with zipfile.ZipFile(archive) as zf:
        return pq.read_table(pa.BufferReader(zf.read("__metadata__")))


def write_raw_flat(output: Path, metadata: pa.Table, sources: dict[str, Path]) -> None:
    """Writes a Flat archive through the C API, which never inspects the
    manifest, so it can hold columns the Python writer refuses."""
    metadata_path = output.with_suffix(".parquet")
    pq.write_table(metadata, metadata_path)
    keep = []
    entries = ffi.new("cozip_entry_t[]", len(sources) + 2)
    for entry, (name, path) in zip(entries, sources.items()):
        keep += [ffi.new("char[]", name.encode()), ffi.new("char[]", str(path).encode())]
        entry.arc_name = keep[-2]
        entry.payload_size = path.stat().st_size
        entry.source.kind = COZIP_SOURCE_PATH
        entry.source.u.path = keep[-1]
    err = ffi.new("cozip_error_t *")
    status = lib.cozip_write_flat(
        str(output).encode(), entries, len(sources), len(sources) + 2,
        str(metadata_path).encode(), err,
    )
    assert status == 0, ffi.string(err.message)


class TestLocalReads:
    def test_reads_every_manifest_column_and_a_location(self, archive: Path) -> None:
        result = cozip.read(archive)
        stored = stored_manifest(archive)

        assert list(result.columns) == [*stored.column_names, "cozip:location"]
        assert result["name"].tolist() == ["a.txt", "dir/b.bin"]
        assert result["category"].tolist() == ["text", "binary"]
        offsets = stored.column("offset").to_pylist()
        sizes = stored.column("size").to_pylist()
        assert result["offset"].tolist() == offsets
        assert result["cozip:location"].tolist() == [
            f"/vsisubfile/{offset}_{size},{archive}"
            for offset, size in zip(offsets, sizes)
        ]

    def test_locations_address_the_stored_bytes(
        self, archive: Path, sources: dict[str, Path]
    ) -> None:
        data = archive.read_bytes()
        result = cozip.read(archive)
        for name, offset, size in zip(result["name"], result["offset"], result["size"]):
            assert data[offset : offset + size] == sources[name].read_bytes()

    def test_location_false_omits_the_column(self, archive: Path) -> None:
        result = cozip.read(archive, location=False)
        assert "cozip:location" not in result.columns
        assert list(result.columns) == stored_manifest(archive).column_names

    def test_projection_keeps_required_columns_first(self, archive: Path) -> None:
        result = cozip.read(archive, columns=["category", 'sensor"name', "category"])
        assert list(result.columns) == [
            "name", "offset", "size", "cozip:location", "category", 'sensor"name',
        ]
        assert result['sensor"name'].tolist() == ["s1", "s2"]

        bare = cozip.read(archive, columns=["category"], location=False)
        assert list(bare.columns) == ["name", "offset", "size", "category"]

    def test_projection_ignores_protected_location_names(self, archive: Path) -> None:
        result = cozip.read(
            archive, columns=["taco:location", "cozip:location", "category"]
        )
        assert list(result.columns) == [
            "name", "offset", "size", "cozip:location", "category",
        ]

    def test_unknown_projected_column_fails(self, archive: Path) -> None:
        with pytest.raises(duckdb.BinderException, match="missing"):
            cozip.read(archive, columns=["missing"])

    def test_accepts_path_objects_and_relative_paths(
        self, archive: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.chdir(archive.parent)
        result = cozip.read(Path(archive.name))
        assert result["cozip:location"].str.endswith(f",{archive.name}").all()

    def test_nullable_columns_keep_duckdb_types(self, archive: Path) -> None:
        result = cozip.read(archive)
        assert str(result["score"].dtype) == "Int32"
        assert str(result["offset"].dtype) == "uint64"

    def test_stored_location_columns_are_replaced(
        self, tmp_path: Path, sources: dict[str, Path]
    ) -> None:
        planned, _ = cozip.stage_metadata(
            pa.table({"name": list(sources), "path": [str(p) for p in sources.values()]})
        )
        forged = (
            planned.append_column("cozip:location", pa.array(["forged", "forged"]))
            .append_column("taco:location", pa.array(["forged", "forged"]))
            .append_column("cozip:gdal_vsi", pa.array(["forged", "forged"]))
        )
        output = tmp_path / "forged.zip"
        write_raw_flat(output, forged, sources)

        result = cozip.read(output)
        assert list(result.columns) == ["name", "offset", "size", "cozip:location"]
        assert result["cozip:location"].str.startswith("/vsisubfile/").all()
        assert "forged" not in result["cozip:location"].tolist()
        assert list(cozip.read(output, location=False).columns) == [
            "name", "offset", "size",
        ]

    def test_hive_style_directories_add_no_columns(
        self, tmp_path: Path, archive: Path
    ) -> None:
        nested = tmp_path / "year=2024" / "out.zip"
        nested.parent.mkdir()
        nested.write_bytes(archive.read_bytes())
        assert list(cozip.read(nested).columns) == list(cozip.read(archive).columns)


class TestRejections:
    def test_rejects_the_taco_profile(self, tmp_path: Path) -> None:
        payload = tmp_path / "payload.bin"
        payload.write_bytes(b"opaque")
        layout = plan(
            files=[("DATA/0/a.bin", payload)],
            priority_names=["COLLECTION.json", "METADATA/level0.parquet"],
        )
        taco = Path(
            write(
                tmp_path / "taco.zip",
                layout,
                priority_files=[
                    ("COLLECTION.json", payload),
                    ("METADATA/level0.parquet", payload),
                ],
            )
        )
        with pytest.raises(cozip.CozipError) as excinfo:
            cozip.read(taco)
        assert excinfo.value.code == COZIP_ERR_PROFILE
        assert excinfo.value.message == (
            "read_flat needs a Flat-profile archive (profile=1). "
            f"Got profile=2 in: {taco}"
        )

    def test_rejects_a_corrupt_index(
        self, tmp_path: Path, archive: Path, range_server
    ) -> None:
        data = bytearray(archive.read_bytes())
        data[43] ^= 0xFF
        corrupt = tmp_path / "corrupt.zip"
        corrupt.write_bytes(bytes(data))
        with pytest.raises(cozip.CozipError, match="cozip integrity hash mismatch") as excinfo:
            cozip.read(corrupt)
        assert excinfo.value.code == COZIP_ERR_INVALID_INDEX
        with pytest.raises(cozip.CozipError) as excinfo:
            cozip.read(f"{range_server.url}/{corrupt.name}?sig=TOPSECRET")
        assert "TOPSECRET" not in str(excinfo.value)
        assert "?<redacted>" in str(excinfo.value)

    def test_a_corrupt_manifest_names_the_archive(
        self, tmp_path: Path, range_server
    ) -> None:
        # Long names make the central directory larger than the hashed tail,
        # so the manifest footer can be damaged without breaking the hash.
        names, paths = [], []
        for i in range(300):
            path = tmp_path / f"{i}.bin"
            path.write_bytes(b"x")
            names.append(f"d/{'n' * 190}{i:04d}.bin")
            paths.append(str(path))
        output = tmp_path / "long.zip"
        cozip.create(output, pa.table({"name": names, "path": paths}))
        data = bytearray(output.read_bytes())
        offset, size = struct.unpack_from("<QQ", data, 51 + 11 + 2 + 12)
        assert offset > 65_536 and offset + size < len(data) - 32_768
        data[offset + size - 4 : offset + size] = b"XXXX"
        output.write_bytes(bytes(data))

        with pytest.raises(duckdb.InvalidInputException) as excinfo:
            cozip.read(output)
        assert f"{output}!__metadata__" in str(excinfo.value)
        assert ".parquet" not in str(excinfo.value)
        with pytest.raises(duckdb.InvalidInputException) as excinfo:
            cozip.read(f"{range_server.url}/{output.name}?sig=TOPSECRET")
        assert "TOPSECRET" not in str(excinfo.value)
        assert "remote archive!__metadata__" in str(excinfo.value)

    def test_rejects_a_plain_parquet_file(self, tmp_path: Path, archive: Path) -> None:
        plain = tmp_path / "manifest.parquet"
        pq.write_table(pa.table({"x": list(range(10_000))}), plain)
        with pytest.raises(cozip.CozipError, match="byte 0 is not a ZIP Local File Header"):
            cozip.read(plain)

    def test_reports_a_missing_file(self, tmp_path: Path) -> None:
        missing = tmp_path / "missing.zip"
        with pytest.raises(cozip.CozipError, match="missing.zip"):
            cozip.read(missing)

    def test_reports_a_missing_url(self, range_server) -> None:
        with pytest.raises(cozip.CozipError, match="404"):
            cozip.read(f"{range_server.url}/missing.zip")


class TestHttpReads:
    def test_matches_the_local_read(self, archive: Path, range_server) -> None:
        url = f"{range_server.url}/{archive.name}"
        remote = cozip.read(url)
        local = cozip.read(archive)

        assert remote.drop(columns="cozip:location").equals(
            local.drop(columns="cozip:location")
        )
        assert remote["cozip:location"].tolist() == [
            f"/vsisubfile/{offset}_{size},/vsicurl/{url}"
            for offset, size in zip(local["offset"], local["size"])
        ]

    def test_reads_only_byte_ranges(self, archive: Path, range_server) -> None:
        cozip.read(f"{range_server.url}/{archive.name}", columns=["category"])
        assert range_server.requests
        assert all(header for method, header in range_server.requests if method == "GET")
        assert archive.stat().st_size > 1_000_000
        assert range_server.served_bytes < 200_000

    def test_projection_over_http(self, archive: Path, range_server) -> None:
        result = cozip.read(
            f"{range_server.url}/{archive.name}", columns=["category"], location=False
        )
        assert list(result.columns) == ["name", "offset", "size", "category"]


class TestIndependence:
    def test_never_installs_or_loads_a_duckdb_extension(
        self, tmp_path: Path, archive: Path
    ) -> None:
        # A fresh HOME has no cached extensions, so a read that needed one
        # would fail or leave ~/.duckdb behind.
        home = tmp_path / "home"
        home.mkdir()
        code = (
            "import sys, cozip, duckdb\n"
            "seen = []\n"
            "connect = duckdb.connect\n"
            "def spy(*args, **kwargs):\n"
            "    con = connect(*args, **kwargs)\n"
            "    seen.append(kwargs.get('config'))\n"
            "    return con\n"
            "duckdb.connect = spy\n"
            "df = cozip.read(sys.argv[1])\n"
            "assert len(df) == 2\n"
            "assert seen == [{'autoinstall_known_extensions': False, "
            "'autoload_known_extensions': False}], seen\n"
        )
        env = {**os.environ, "HOME": str(home), "USERPROFILE": str(home)}
        subprocess.run([sys.executable, "-c", code, str(archive)], check=True, env=env)
        assert not (home / ".duckdb").exists()


@pytest.mark.parametrize(
    ("source", "base"),
    [
        ("data/out.zip", "data/out.zip"),
        ("https://host/out.zip", "/vsicurl/https://host/out.zip"),
        ("s3://bucket/out.zip", "/vsis3/bucket/out.zip"),
        ("gs://bucket/out.zip", "/vsigs/bucket/out.zip"),
        ("az://container/out.zip", "/vsiaz/container/out.zip"),
        ("azure://container/out.zip", "/vsiaz/container/out.zip"),
        (
            "hf://datasets/owner/repo@v2/out.zip",
            "/vsicurl/https://huggingface.co/datasets/owner/repo/resolve/v2/out.zip",
        ),
    ],
)
def test_location_base(source: str, base: str) -> None:
    assert _location_base(source) == base


def test_location_base_rejects_an_unparseable_hf_url() -> None:
    with pytest.raises(cozip.CozipError, match="cannot parse hf:// URL"):
        _location_base("hf://owner")
