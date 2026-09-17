"""Check local cozip archives against SPEC.md Part I, standard library only.

Usage: python check_cozip.py ARCHIVE [ARCHIVE ...]

Prints one line per archive and exits with status 1 if any archive fails. The checks
follow the reader procedure in spec 8.5, the index layout in 7 and the ZIP rules in 5.1:
the 51-byte __cozip__ header, the integrity hash, every index section, and each indexed
range against the Central Directory. The Parquet manifest itself is not opened.
"""

import os
import struct
import sys
import zipfile

INDEX_OFFSET, WINDOW = 51, 32768
FNV_BASIS, FNV_PRIME, MASK = 0xCBF29CE484222325, 0x100000001B3, (1 << 64) - 1
FORBIDDEN_FLAGS = 0x2049  # bits 0, 3, 6 and 13


class CozipInvalid(Exception):
    def __init__(self, code, detail):
        super().__init__(f"{code}: {detail}")
        self.code = code


def fnv1a64(data, h=FNV_BASIS):
    for byte in data:
        h = ((h ^ byte) * FNV_PRIME) & MASK
    return h


def invalid_name(name):
    """Spec 5.3, except the trailing slash, which callers classify themselves."""
    raw = name.encode("utf-8")
    return (not raw or any(b == 0 or b >= 0x80 for b in raw) or name.startswith("/")
            or "\\" in name or (len(name) > 1 and name[0].isalpha() and name[1] == ":")
            or any(part in (".", "..") for part in name.split("/")))


def read_at(f, offset, length):
    f.seek(offset)
    return f.read(length)


def check(path):
    size = os.path.getsize(path)
    if size < WINDOW + INDEX_OFFSET:
        raise CozipInvalid("ARCHIVE_TOO_SMALL", f"{size} bytes")
    with open(path, "rb") as f:
        head = read_at(f, 0, INDEX_OFFSET)
        sig, _, flags, method, _, _, _, csize, usize, nlen, xlen = struct.unpack_from(
            "<IHHHHHIIIHH", head)
        if (sig != 0x04034B50 or flags & FORBIDDEN_FLAGS or method != 0 or csize != usize
                or csize in (0, 0xFFFFFFFF) or nlen != 9 or xlen != 12
                or head[30:43] != b"__cozip__\x0c\xca\x08\x00"):
            raise CozipInvalid("INVALID_LFH", "byte 0 is not the 51-byte __cozip__ header")
        if INDEX_OFFSET + csize > size:
            raise CozipInvalid("TRUNCATED_INDEX", f"{csize}-byte index in {size} bytes")
        index = read_at(f, INDEX_OFFSET, csize)
        resume = max(INDEX_OFFSET + csize, size - WINDOW)
        stored = struct.unpack_from("<Q", head, 43)[0]
        if fnv1a64(read_at(f, resume, size - resume), fnv1a64(index)) != stored:
            raise CozipInvalid("HASH_MISMATCH", f"stored 0x{stored:016x}")
        if csize < 11 or index[:4] != b"CZIP":
            raise CozipInvalid("INVALID_MAGIC", repr(index[:4]))
        version, profile, count = struct.unpack_from("<HBI", index, 4)
        if version > 1:
            raise CozipInvalid("UNSUPPORTED_VERSION", str(version))
        if 11 + 18 * count > csize:
            raise CozipInvalid("TRUNCATED_INDEX", f"{count} entries in {csize} bytes")
        lengths = struct.unpack_from(f"<{count}H", index, 11)
        cursor = 11 + 2 * count
        if 0 in lengths or cursor + sum(lengths) + 16 * count != csize:
            raise CozipInvalid("TRUNCATED_INDEX", "sections do not add up to the payload")
        names = []
        for length in lengths:
            names.append(index[cursor:cursor + length].decode("latin-1"))
            cursor += length
        offsets = struct.unpack_from(f"<{count}Q", index, cursor)
        sizes = struct.unpack_from(f"<{count}Q", index, cursor + 8 * count)
        priorities = dict(zip(names, zip(offsets, sizes)))
        if len(priorities) != count:
            raise CozipInvalid("DUPLICATE_NAME", "the index lists a name twice")

        try:
            with zipfile.ZipFile(path) as archive:
                infos, comment = archive.infolist(), archive.comment
        except zipfile.BadZipFile as exc:
            raise CozipInvalid("INVALID_ZIP_STRUCTURE", str(exc)) from exc
        if comment or not infos or infos[0].filename != "__cozip__" or infos[0].header_offset:
            raise CozipInvalid("INVALID_ZIP_STRUCTURE", "comment present or __cozip__ not first")
        entries = {}
        for info in infos:
            name = info.filename
            if name in entries:
                raise CozipInvalid("DUPLICATE_NAME", name)
            if invalid_name(name):
                raise CozipInvalid("INVALID_NAME", name)
            if (name.endswith("/") or info.compress_type != 0 or info.flag_bits & FORBIDDEN_FLAGS
                    or info.file_size == 0 or info.compress_size != info.file_size):
                raise CozipInvalid("INVALID_ZIP_STRUCTURE", name)
            lfh = read_at(f, info.header_offset, 30)
            if len(lfh) != 30 or lfh[:4] != b"PK\x03\x04":
                raise CozipInvalid("INVALID_ZIP_STRUCTURE", f"no local header for {name}")
            nlen, xlen = struct.unpack_from("<HH", lfh, 26)
            entries[name] = (info.header_offset + 30 + nlen + xlen, info.file_size)

    for name, (offset, length) in priorities.items():
        if invalid_name(name) or name.endswith("/") or name in ("__cozip__", "__cozip_padding__"):
            raise CozipInvalid("INVALID_NAME", name)
        if offset + length > size:
            raise CozipInvalid("INVALID_OFFSET", name)
        if entries.get(name) != (offset, length):
            raise CozipInvalid("MISSING_ENTRY", name)
    if profile == 1 and set(priorities) != {"__metadata__"}:
        raise CozipInvalid("MISSING_ENTRY", "a Flat index lists exactly __metadata__")
    return profile, priorities, entries


def main(targets):
    failed = False
    for target in targets:
        try:
            profile, priorities, entries = check(target)
        except CozipInvalid as exc:
            failed = True
            print(f"{target}: {exc}")
        else:
            print(f"{target}: ok, profile {profile}, {len(entries)} ZIP entries, "
                  f"priority files {sorted(priorities)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
