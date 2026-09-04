"""Python adapter for libcozip's TACO writer ABI."""

from __future__ import annotations

import os
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ._core import CozipError, ffi, lib

__all__ = ["API_VERSION", "Plan", "PlannedFile", "plan", "write"]

API_VERSION = 1

FilePair = tuple[str, str | os.PathLike[str]]


@dataclass(frozen=True)
class PlannedFile:
    """C-computed physical location of one non-priority source file."""

    name: str
    source: Path
    offset: int
    size: int


@dataclass(frozen=True, repr=False)
class _NativePlan:
    struct_size: int
    abi_version: int
    n_files: int
    n_priorities: int
    layout_hash: int


@dataclass(frozen=True)
class Plan:
    """Immutable result passed from ``plan()`` back to ``write()``."""

    files: tuple[PlannedFile, ...]
    priority_names: tuple[str, ...]
    _native: _NativePlan = field(repr=False)

    @property
    def offsets(self) -> dict[str, tuple[int, int]]:
        """Return ``archive_name -> (payload_offset, payload_size)``."""
        return {item.name: (item.offset, item.size) for item in self.files}


def _check(status: int, err: Any) -> None:
    if status != 0:
        raise CozipError.from_struct(err)


def _normalize_name(value: Any, *, context: str) -> str:
    if not isinstance(value, str):
        raise TypeError(f"cozip._taco: {context} must be a string")
    if "\0" in value:
        raise ValueError(f"cozip._taco: NUL is forbidden in {context}")
    # Encoding here is only ABI marshaling. libcozip performs the physical
    # name validation shared by every language binding.
    value.encode("utf-8")
    return value


def _normalize_path(value: Any, *, context: str) -> Path:
    try:
        raw = os.fspath(value)
    except TypeError as exc:
        raise TypeError(f"cozip._taco: {context} must be path-like") from exc
    if not isinstance(raw, str):
        raise TypeError(f"cozip._taco: {context} must resolve to a string path")
    if not raw:
        raise ValueError(f"cozip._taco: {context} must be a non-empty path")
    if "\0" in raw:
        raise ValueError(f"cozip._taco: {context} contains a NUL byte")
    return Path(raw).expanduser().resolve()


def _normalize_file_pairs(
    values: Sequence[FilePair], *, context: str
) -> tuple[tuple[str, Path], ...]:
    if isinstance(values, (str, bytes)) or not isinstance(values, Sequence):
        raise TypeError(
            f"cozip._taco: {context} must be an ordered sequence of "
            "(archive_name, source_path) pairs"
        )

    result: list[tuple[str, Path]] = []
    for index, item in enumerate(values):
        if (
            isinstance(item, (str, bytes))
            or not isinstance(item, Sequence)
            or len(item) != 2
        ):
            raise ValueError(
                f"cozip._taco: {context}[{index}] must be an "
                "(archive_name, source_path) pair"
            )
        raw_name, raw_source = item
        name = _normalize_name(raw_name, context=f"{context}[{index}] name")
        source = _normalize_path(raw_source, context=f"{context}[{index}] source")
        result.append((name, source))
    return tuple(result)


def _normalize_priority_names(values: Sequence[str]) -> tuple[str, ...]:
    if isinstance(values, (str, bytes)) or not isinstance(values, Sequence):
        raise TypeError("cozip._taco: priority_names must be an ordered sequence")
    return tuple(
        _normalize_name(value, context=f"priority_names[{index}]")
        for index, value in enumerate(values)
    )


def _allocate_path_entries(
    pairs: Sequence[tuple[str, Path | None]],
    *,
    planned: Sequence[PlannedFile] | None = None,
) -> tuple[Any, list[Any]]:
    if not pairs:
        return ffi.NULL, []

    entries = ffi.new(f"cozip_path_entry_t[{len(pairs)}]")
    keepalive: list[Any] = []
    for index, (name_value, source_value) in enumerate(pairs):
        name = ffi.new("char[]", name_value.encode("utf-8"))
        keepalive.append(name)
        entries[index].arc_name = name
        if source_value is not None:
            source = ffi.new("char[]", str(source_value).encode("utf-8"))
            keepalive.append(source)
            entries[index].source_path = source
        if planned is not None:
            entries[index].payload_offset = planned[index].offset
            entries[index].payload_size = planned[index].size
    return entries, keepalive


def plan(files: Sequence[FilePair], priority_names: Sequence[str]) -> Plan:
    """Ask libcozip to plan DATA offsets using priority-name placeholders.

    Non-priority ``files`` must already exist. Priority payloads need not exist
    yet: only their ordered names are required because names determine the
    byte-0 index size. TACO uses ``Plan.offsets`` to materialize its own
    metadata files afterward.
    """
    normalized_files = _normalize_file_pairs(files, context="files")
    normalized_priorities = _normalize_priority_names(priority_names)

    data_entries, data_keepalive = _allocate_path_entries(normalized_files)
    priority_entries, priority_keepalive = _allocate_path_entries(
        [(name, None) for name in normalized_priorities]
    )
    native_plan = ffi.new("cozip_taco_plan_t *")
    native_plan.struct_size = ffi.sizeof("cozip_taco_plan_t")
    native_plan.abi_version = API_VERSION
    err = ffi.new("cozip_error_t *")
    _check(
        lib.cozip_plan_taco(
            data_entries,
            len(normalized_files),
            priority_entries,
            len(normalized_priorities),
            native_plan,
            err,
        ),
        err,
    )

    planned_files = tuple(
        PlannedFile(
            name=name,
            source=source,
            offset=int(data_entries[index].payload_offset),
            size=int(data_entries[index].payload_size),
        )
        for index, (name, source) in enumerate(normalized_files)
    )
    token = _NativePlan(
        struct_size=int(native_plan.struct_size),
        abi_version=int(native_plan.abi_version),
        n_files=int(native_plan.n_files),
        n_priorities=int(native_plan.n_priorities),
        layout_hash=int(native_plan.layout_hash),
    )
    # C pointers only need to survive the native call above.
    _ = data_keepalive, priority_keepalive
    return Plan(planned_files, normalized_priorities, token)


def write(
    output: str | os.PathLike[str],
    layout: Plan,
    priority_files: Sequence[FilePair],
) -> str:
    """Write already-materialized payloads through libcozip profile 2.

    ``priority_files`` must use exactly the ordered names supplied to
    ``plan()``. Cozip treats every source as opaque bytes. Publication policy
    (temporary destination, overwrite, atomic rename) remains TACO's concern.
    """
    if not isinstance(layout, Plan):
        raise TypeError("cozip._taco: layout must be returned by plan()")

    normalized_priorities = _normalize_file_pairs(
        priority_files, context="priority_files"
    )
    actual_names = tuple(name for name, _ in normalized_priorities)
    if actual_names != layout.priority_names:
        raise ValueError(
            "cozip._taco: priority_files names/order differ from plan; "
            f"planned={list(layout.priority_names)!r}, "
            f"actual={list(actual_names)!r}"
        )

    data_pairs = [(item.name, item.source) for item in layout.files]
    data_entries, data_keepalive = _allocate_path_entries(
        data_pairs, planned=layout.files
    )
    priority_entries, priority_keepalive = _allocate_path_entries(normalized_priorities)

    native_plan = ffi.new("cozip_taco_plan_t *")
    native_plan.struct_size = layout._native.struct_size
    native_plan.abi_version = layout._native.abi_version
    native_plan.n_files = layout._native.n_files
    native_plan.n_priorities = layout._native.n_priorities
    native_plan.layout_hash = layout._native.layout_hash

    output_path = _normalize_path(output, context="output")
    output_c = ffi.new("char[]", str(output_path).encode("utf-8"))
    err = ffi.new("cozip_error_t *")
    _check(
        lib.cozip_write_taco(
            output_c,
            data_entries,
            len(data_pairs),
            priority_entries,
            len(normalized_priorities),
            native_plan,
            err,
        ),
        err,
    )
    _ = data_keepalive, priority_keepalive, output_c
    return str(output_path)
