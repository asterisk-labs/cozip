"""Tag the wheel as platform-specific but ABI-agnostic.

cffi ABI mode loads the native library via libffi at runtime, so the
wheel works on any Python 3.x — but the bundled .dylib is for one
specific OS+arch.
"""

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version: str, build_data: dict) -> None:
        # Force a platform-tagged wheel (e.g. py3-none-macosx_11_0_arm64)
        # instead of the default py3-none-any.
        build_data["pure_python"] = False
        build_data["infer_tag"] = True