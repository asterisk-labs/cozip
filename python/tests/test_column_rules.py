"""Column names, manifest types and the single plan of create().

DuckDB, which decodes manifests, compares column names without case, so the
writers and readers treat names that differ only in case as the same name.
"""

from pathlib import Path

import cozip
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from cozip import _writer


@pytest.fixture
def sources(tmp_path: Path) -> list[Path]:
    paths = []
    for i, size in enumerate((10, 20, 30)):
        path = tmp_path / f"f{i}.bin"
        path.write_bytes(bytes([i]) * size)
        paths.append(path)
    return paths


def _table(sources: list[Path], **extras) -> pa.Table:
    return pa.table(
        {
            "name": [p.name for p in sources],
            "path": [str(p) for p in sources],
            **extras,
        }
    )


def _write_unchecked(out: Path, manifest: pa.Table, sources: list[Path]) -> None:
    """Writes an archive around a manifest the Python checks would refuse,
    as another producer could."""
    parquet = out.with_suffix(".parquet")
    pq.write_table(manifest, parquet)
    _writer._write(
        str(out), [p.name for p in sources], [str(p) for p in sources], None, str(parquet)
    )


class TestReader:
    def test_stored_location_in_any_case_is_dropped(
        self, tmp_path: Path, sources: list[Path]
    ) -> None:
        manifest, _ = cozip.stage_metadata(_table(sources))
        forged = ["/vsicurl/https://evil.example/x"] * len(sources)
        manifest = manifest.append_column("COZIP:LOCATION", pa.array(forged))
        archive = tmp_path / "forged.zip"
        _write_unchecked(archive, manifest, sources)

        result = cozip.read(archive)
        assert list(result.columns) == ["name", "offset", "size", "cozip:location"]
        assert result["cozip:location"][0].startswith("/vsisubfile/")
        assert list(cozip.read(archive, location=False).columns) == [
            "name",
            "offset",
            "size",
        ]

    def test_columns_that_differ_only_in_case_are_refused(
        self, tmp_path: Path, sources: list[Path]
    ) -> None:
        manifest, _ = cozip.stage_metadata(_table(sources))
        manifest = manifest.append_column("label", pa.array(["a"] * 3))
        manifest = manifest.append_column("LABEL", pa.array(["b"] * 3))
        archive = tmp_path / "case.zip"
        _write_unchecked(archive, manifest, sources)
        with pytest.raises(ValueError, match="differ only in case"):
            cozip.read(archive)

    def test_columns_match_exactly(self, tmp_path: Path, sources: list[Path]) -> None:
        archive = tmp_path / "a.zip"
        cozip.write(archive, _table(sources, split=["train", "val", "train"]))
        assert list(cozip.read(archive, columns=["split", "split"]).columns) == [
            "name",
            "offset",
            "size",
            "cozip:location",
            "split",
        ]
        with pytest.raises(ValueError, match=r"unknown column\(s\) \['Name'\]"):
            cozip.read(archive, columns=["Name"])
        # Location names in any case are ignored, as the location is computed.
        assert "COZIP:LOCATION" not in cozip.read(archive, columns=["COZIP:LOCATION"])


class TestWriter:
    @pytest.mark.parametrize("column", ["Offset", "SIZE", "Cozip:Location", "TACO:location"])
    def test_reserved_columns_in_any_case_are_refused(
        self, sources: list[Path], column: str
    ) -> None:
        with pytest.raises(ValueError, match="reserved column"):
            cozip.stage_metadata(_table(sources, **{column: [1, 2, 3]}))

    def test_input_columns_that_differ_only_in_case_are_refused(
        self, sources: list[Path]
    ) -> None:
        with pytest.raises(ValueError, match="differ only in case"):
            cozip.stage_metadata(_table(sources, label=[1, 2, 3], Label=[4, 5, 6]))

    def test_repeated_input_column_is_refused(self, sources: list[Path]) -> None:
        table = _table(sources).append_column("name", pa.array(["x", "y", "z"]))
        with pytest.raises(ValueError, match="more than once"):
            cozip.stage_metadata(table)

    def test_a_dataframe_gets_a_conversion_hint(self, sources: list[Path]) -> None:
        df = pd.DataFrame({"name": [p.name for p in sources], "path": [str(p) for p in sources]})
        with pytest.raises(TypeError, match="from_pandas"):
            cozip.stage_metadata(df)

    def test_pandas_index_stays_out_of_the_manifest(self, sources: list[Path]) -> None:
        df = pd.DataFrame(
            {"name": [p.name for p in sources], "path": [str(p) for p in sources]},
            index=[5, 9, 11],
        )
        manifest, _ = cozip.stage_metadata(pa.Table.from_pandas(df))
        assert manifest.column_names == ["name", "offset", "size"]
        assert b"pandas" not in (manifest.schema.metadata or {})

    @pytest.mark.parametrize(
        ("column", "values", "message"),
        [
            ("offset", pa.array([1, 2, 3], pa.int64()), "'offset' must be uint64"),
            ("size", pa.array([1.0, 2.0, 3.0]), "'size' must be uint64"),
            ("name", pa.array([b"a", b"b", b"c"]), "'name' must be a string"),
        ],
    )
    def test_manifest_types_follow_the_spec(
        self, tmp_path: Path, sources: list[Path], column: str, values, message: str
    ) -> None:
        manifest, paths = cozip.stage_metadata(_table(sources))
        manifest = manifest.set_column(manifest.schema.get_field_index(column), column, values)
        parquet = tmp_path / "m.parquet"
        pq.write_table(manifest, parquet)
        with pytest.raises(ValueError, match=message):
            cozip.stage_create(tmp_path / "out.zip", paths, parquet)

    def test_create_refuses_a_source_that_changed_after_planning(
        self, tmp_path: Path, sources: list[Path], monkeypatch: pytest.MonkeyPatch
    ) -> None:
        write_table = pq.write_table

        def grow_then_write(*args, **kwargs):
            with sources[0].open("ab") as stream:
                stream.write(b"grown")
            return write_table(*args, **kwargs)

        monkeypatch.setattr(_writer.pq, "write_table", grow_then_write)
        out = tmp_path / "changed.zip"
        with pytest.raises(cozip.CozipError, match="did the file change"):
            cozip.write(out, _table(sources))
        assert not out.exists()

    def test_create_replaces_a_read_only_file(
        self, tmp_path: Path, sources: list[Path]
    ) -> None:
        out = tmp_path / "ro.zip"
        out.write_bytes(b"precious")
        out.chmod(0o444)
        cozip.write(out, _table(sources))
        assert len(cozip.read(out)) == len(sources)
        assert not list(tmp_path.glob("*.cozip-tmp-*"))
