"""Build and package the native library used by the CFFI bindings."""

from __future__ import annotations

import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from hatchling.builders.hooks.plugin.interface import BuildHookInterface
from packaging.tags import sys_tags


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version: str, build_data: dict) -> None:
        root = Path(self.root)
        lib_dir = root / "cozip" / "_lib"
        lib_path = lib_dir / _library_name()

        # Release wheels place a CI-built library here before invoking Hatch.
        # Source builds compile the copy of core/ carried by the sdist.
        if not lib_path.is_file():
            _compile_native(_native_source(root), lib_path)

        # CFFI uses dlopen rather than the CPython extension ABI. The wheel is
        # py3-none, but it is still tied to an OS and architecture.
        build_data["pure_python"] = False
        build_data["tag"] = f"py3-none-{_platform_tag()}"


def _library_name() -> str:
    if sys.platform == "darwin":
        return "cozip.dylib"
    if sys.platform == "win32":
        return "cozip.dll"
    return "cozip.so"


def _platform_tag() -> str:
    if sys.platform == "darwin":
        machine = platform.machine().lower()
        if machine == "aarch64":
            machine = "arm64"
        return f"macosx_11_0_{machine}"
    return next(
        tag.platform
        for tag in sys_tags()
        if "manylinux" not in tag.platform and "musllinux" not in tag.platform
    )


def _native_source(root: Path) -> Path:
    checkout_core = root.parent / "core"
    if (checkout_core / "CMakeLists.txt").is_file():
        return checkout_core

    sdist_core = root / "native"
    if (sdist_core / "CMakeLists.txt").is_file():
        return sdist_core

    raise RuntimeError("cozip native sources are missing from the source tree")


def _compile_native(source_dir: Path, destination: Path) -> None:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        raise RuntimeError("building cozip from source requires CMake and Ninja")

    with tempfile.TemporaryDirectory(prefix="cozip-native-") as temp:
        build_dir = Path(temp)
        subprocess.run(
            [
                cmake,
                "-S",
                str(source_dir),
                "-B",
                str(build_dir),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCOZIP_BUILD_TESTS=OFF",
                "-DCOZIP_INSTALL=OFF",
            ],
            check=True,
        )
        subprocess.run(
            [cmake, "--build", str(build_dir), "--target", "cozip"],
            check=True,
        )

        built = build_dir / _library_name()
        if not built.is_file():
            raise RuntimeError(f"native build did not produce {built.name}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(built, destination)
