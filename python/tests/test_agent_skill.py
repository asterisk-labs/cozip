"""The repository's agent skill tracks the release and its checker still works."""

import re
import types
from pathlib import Path

import cozip
import pyarrow as pa
import pytest

ROOT = Path(__file__).resolve().parents[2]
SKILL = ROOT / ".claude" / "skills" / "cozip"
CODEX_SKILL = ROOT / ".agents" / "skills" / "cozip"

# Release CI copies python/tests next to an installed wheel, away from the checkout.
pytestmark = pytest.mark.skipif(
    not (SKILL / "SKILL.md").is_file(), reason="needs a cozip checkout"
)


def _load_checker() -> types.ModuleType:
    # Executed from source so no __pycache__ lands inside the skill directory.
    path = SKILL / "scripts" / "check_cozip.py"
    module = types.ModuleType("check_cozip")
    module.__file__ = str(path)
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), module.__dict__)
    return module


def test_agent_skill_tracks_the_release_and_its_links_resolve() -> None:
    body = (SKILL / "SKILL.md").read_text(encoding="utf-8")
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

    assert f"This skill describes **cozip {version}**" in body
    assert CODEX_SKILL.resolve() == SKILL.resolve()

    failures = []
    for source in SKILL.rglob("*.md"):
        text = source.read_text(encoding="utf-8")
        for target in re.findall(r"\]\((?![a-z]+://)([^)#\s]+)(?:#[^)]*)?\)", text):
            if not (source.parent / target).exists():
                failures.append(f"{source.relative_to(SKILL)}: {target}")

    assert not failures, "broken skill links:\n" + "\n".join(failures)


def test_agent_skill_checker_accepts_a_fresh_archive(tmp_path: Path) -> None:
    checker = _load_checker()
    source = tmp_path / "a.txt"
    source.write_bytes(b"hello cozip\n" * 8)
    archive = Path(
        cozip.write(
            tmp_path / "out.zip", pa.table({"name": ["a.txt"], "path": [str(source)]})
        )
    )

    profile, priorities, entries = checker.check(archive)
    assert profile == 1
    assert set(priorities) == {"__metadata__"}
    assert entries["a.txt"] == (51 + 41 + 30 + len("a.txt"), 96)

    corrupt = tmp_path / "corrupt.zip"
    data = bytearray(archive.read_bytes())
    data[60] ^= 1
    corrupt.write_bytes(bytes(data))
    with pytest.raises(checker.CozipInvalid) as captured:
        checker.check(corrupt)
    assert captured.value.code == "HASH_MISMATCH"
