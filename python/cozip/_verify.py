"""Verify the byte-0 index and the integrity hash of any cozip archive."""
import os


def check_source(source: str | os.PathLike[str], caller: str) -> str:
    """The source as ASCII text, or the error a read of it would raise."""
    source = os.fspath(source)
    if not isinstance(source, str):
        raise TypeError(f"{caller}: source must be a string path or URL")
    if not source:
        raise ValueError(f"{caller}: source must be a non-empty ASCII path or URL")
    if "\0" in source:
        raise ValueError(f"{caller}: source contains a NUL byte")
    if not source.isascii():
        raise ValueError(
            f"{caller}: source contains non-ASCII characters; "
            "cozip supports ASCII paths and URLs only"
        )
    return source


def verify(source: str | os.PathLike[str]) -> None:
    """Check a cozip archive's access path, whatever its profile.

    Checks the header, index, indexed names and ranges, integrity hash and ZIP
    end record (spec 8.5 and 7). It does not parse the Central Directory,
    check payloads or enforce profile rules. Returns ``None`` when these
    checks pass; otherwise raises ``CozipError`` with the failing rule's name.
    """
    source = check_source(source, "cozip.verify")
    from ._core import CozipError, ffi, lib

    message = ffi.new("char **")
    status = lib.cozip_verify(source.encode("ascii"), message)
    try:
        if status != 0:
            raise CozipError.from_message(status, message[0])
    finally:
        lib.cozip_free(message[0])
