from importlib.metadata import PackageNotFoundError, version

from ._core import CozipError, ffi, lib
from ._writer import create, stage_create, stage_metadata
from ._reader import read

try:
    __version__ = version("cozip")
except PackageNotFoundError:
    __version__ = "0.0.0+unknown"

__all__ = [
    "CozipError",
    "create",
    "stage_metadata",
    "stage_create",
    "read",
    "ffi",
    "lib",
    "__version__",
]