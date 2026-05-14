"""cffi bindings to libcozip. Loads the bundled shared library at import time"""

import os
import sys
from pathlib import Path

import cffi

# Status codes. Must match cozip.h
COZIP_OK                    = 0
COZIP_ERR_INVALID_LFH       = 1
COZIP_ERR_ARCHIVE_TOO_SMALL = 2
COZIP_ERR_INVALID_ARGUMENT  = 100
COZIP_ERR_BUFFER_TOO_SMALL  = 101
COZIP_ERR_IO                = 102

# Profile selector for cozip_build_index_payload
COZIP_PROFILE_NONE = 0
COZIP_PROFILE_FLAT = 1
COZIP_PROFILE_TACO = 2

# Source kind for cozip_entry_t.source.kind
COZIP_SOURCE_NONE   = 0
COZIP_SOURCE_PATH   = 1
COZIP_SOURCE_BUFFER = 2


ffi = cffi.FFI()
ffi.cdef("""
typedef struct { int code; char message[192]; } cozip_error_t;

typedef struct {
    int kind;
    union {
        const char* path;
        struct { const uint8_t* data; size_t size; } buffer;
    } u;
} cozip_source_t;

typedef struct {
    const char* arc_name;
    uint64_t payload_size;
    bool in_index;
    cozip_source_t source;
    uint64_t lfh_offset, lfh_size, payload_offset;
} cozip_entry_t;

const char* cozip_status_string(int status);
const char* cozip_version_string(void);
const char* cozip_index_name(void);
const char* cozip_padding_name(void);
const char* cozip_flat_metadata_name(void);

int cozip_plan(cozip_entry_t*, size_t, cozip_error_t*);
int cozip_index_payload_size(const cozip_entry_t*, size_t,
                             size_t*, cozip_error_t*);
int cozip_build_index_payload(const cozip_entry_t*, size_t, int,
                              uint8_t*, size_t, cozip_error_t*);
int cozip_write_archive(const char*, const cozip_entry_t*, size_t,
                        const uint8_t*, size_t, cozip_error_t*);
int cozip_patch_integrity_hash(const char*, size_t, cozip_error_t*);

uint64_t cozip_predict_zip32_archive_size(const cozip_entry_t*, size_t,
                                          size_t);
uint64_t cozip_required_padding_payload(uint64_t);

int cozip_plan_flat(cozip_entry_t*, size_t, cozip_error_t*);
int cozip_write_flat(const char*, cozip_entry_t*, size_t, size_t,
                     const char*, cozip_error_t*);
""")


def _lib_filename() -> str:
    """Shared library filename for the current platform.

    No 'lib' prefix because CMakeLists sets PREFIX "" — artifact name
    is identical across Linux/macOS/Windows.
    """
    if sys.platform == "darwin":
        return "cozip.dylib"
    if sys.platform == "win32":
        return "cozip.dll"
    return "cozip.so"


def _resolve_lib_path() -> str:
    """Find the shared library: env override, then bundled location."""
    name = _lib_filename()

    env = os.environ.get("COZIP_LIB_PATH")
    if env:
        if not Path(env).exists():
            raise ImportError(f"cozip: COZIP_LIB_PATH={env!r} does not exist")
        return env

    canonical = Path(__file__).resolve().parent / "_lib" / name
    if canonical.exists():
        return str(canonical)

    raise ImportError(
        f"cozip: native library {name!r} not found at {canonical}.\n"
        f"  Fix: run `make py-install` from the cozip/ directory, "
        f"or set COZIP_LIB_PATH=/abs/path/to/{name}"
    )


lib = ffi.dlopen(_resolve_lib_path())


class CozipError(RuntimeError):
    """Raised when libcozip returns a non-zero status."""

    def __init__(self, code: int, name: str, message: str):
        super().__init__(f"[{name}] {message}")
        self.code = code
        self.name = name
        self.message = message

    @classmethod
    def from_struct(cls, err) -> "CozipError":
        """Builds the exception from a cozip_error_t struct."""
        code = int(err.code)
        msg = ffi.string(err.message).decode("utf-8", errors="replace")
        name = ffi.string(lib.cozip_status_string(code)).decode("ascii")
        return cls(code, name, msg)