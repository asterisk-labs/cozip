import pytest


@pytest.fixture(autouse=True)
def _clean_extension_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("COZIP_EXTENSION", raising=False)
