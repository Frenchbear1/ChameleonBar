"""Build a Microsoft .res icon resource without requiring the Windows SDK."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def _ordinal(value: int) -> bytes:
    return struct.pack("<HH", 0xFFFF, value)


def _pad4(value: bytes) -> bytes:
    return value + bytes((-len(value)) % 4)


def _resource(resource_type: int, name: int, data: bytes) -> bytes:
    variable = _pad4(_ordinal(resource_type) + _ordinal(name))
    fixed = struct.pack(
        "<IHHII", 0, 0x1030, 0x0409, 0, 0
    )  # data version, flags, language, version, characteristics
    header = variable + fixed
    return (
        struct.pack("<II", len(data), 8 + len(header))
        + header
        + _pad4(data)
    )


def build_resource(icon_path: Path, output_path: Path, group_id: int = 101) -> None:
    icon = icon_path.read_bytes()
    reserved, icon_type, count = struct.unpack_from("<HHH", icon, 0)
    if reserved != 0 or icon_type != 1 or count < 1:
        raise ValueError(f"{icon_path} is not a valid Windows icon")

    entries: list[tuple[bytes, bytes]] = []
    for index in range(count):
        entry = icon[6 + index * 16 : 6 + (index + 1) * 16]
        width, height, colors, entry_reserved, planes, bits, size, offset = (
            struct.unpack("<BBBBHHII", entry)
        )
        image = icon[offset : offset + size]
        if len(image) != size:
            raise ValueError(f"Icon image {index} is truncated")
        group_entry = struct.pack(
            "<BBBBHHIH",
            width,
            height,
            colors,
            entry_reserved,
            planes,
            bits,
            size,
            index + 1,
        )
        entries.append((group_entry, image))

    # A conventional empty entry marks the file as a Win32 .res stream.
    output = _resource(0, 0, b"")
    for resource_id, (_, image) in enumerate(entries, start=1):
        output += _resource(3, resource_id, image)  # RT_ICON
    group = struct.pack("<HHH", 0, 1, count) + b"".join(
        group_entry for group_entry, _ in entries
    )
    output += _resource(14, group_id, group)  # RT_GROUP_ICON
    output_path.write_bytes(output)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_icon_resource.py INPUT.ico OUTPUT.res")
    build_resource(Path(sys.argv[1]), Path(sys.argv[2]))
