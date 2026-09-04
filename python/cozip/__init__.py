"""Public Python API for cozip."""

from importlib.metadata import PackageNotFoundError, version

from ._core import CozipError, ffi, lib

try:
    __version__ = version("cozip")
except PackageNotFoundError:
    __version__ = "0.0.0+unknown"

__all__ = [
    "CozipError",
    "__version__",
    "create",
    "ffi",
    "lib",
    "read",
    "stage_create",
    "stage_metadata",
    "write",
]

_WRITER_NAMES = frozenset({"create", "write", "stage_metadata", "stage_create"})


def __getattr__(name: str):
    if name in _WRITER_NAMES:
        from ._writer import create, stage_create, stage_metadata

        globals().update(
            create=create,
            write=create,
            stage_metadata=stage_metadata,
            stage_create=stage_create,
        )
        return globals()[name]
    if name == "read":
        from ._reader import read

        globals()["read"] = read
        return read
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def __dir__() -> list[str]:
    return sorted(set(globals()) | set(__all__))
