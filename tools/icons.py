# SPDX-License-Identifier: MIT
"""Pack the PNGs tools/make-icons rendered into a Windows .ico and a macOS
.icns. Both formats hold PNG images as they are; no image library needed.

    python3 tools/icons.py OUT_DIR ICON

reads OUT_DIR/ICON/png/<size>.png, writes OUT_DIR/ICON.ico and ICON.icns.
"""
import struct
import sys
from pathlib import Path


def png(out: Path, size: int) -> bytes:
    return (out / "png" / f"{size}.png").read_bytes()


def ico(out: Path) -> bytes:
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [png(out, s) for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        dim = 0 if s >= 256 else s  # 0 means 256
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return head + entries + b"".join(images)


def icns(out: Path) -> bytes:
    # The PNG-holding types, by pixel size.
    types = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128),
             (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024)]
    body = b""
    for kind, s in types:
        data = png(out, s)
        body += kind + struct.pack(">I", 8 + len(data)) + data
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def main() -> None:
    out, name = Path(sys.argv[1]), sys.argv[2]
    (out / f"{name}.ico").write_bytes(ico(out / name))
    (out / f"{name}.icns").write_bytes(icns(out / name))


if __name__ == "__main__":
    main()
