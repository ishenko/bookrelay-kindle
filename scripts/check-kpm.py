#!/usr/bin/env python3
import json
import base64
import re
import stat
import struct
import zlib
from pathlib import Path

manifest = json.loads(Path("packaging/kpm/manifest.json").read_text())
assert manifest["manifest_version"] == 2
assert manifest["id"] == "bookrelay-kindle"
assert manifest["version"] == [0, 1, 19]
assert manifest["supported_platforms"] == ["kindlehf"]
assert manifest["bookrelay_min_firmware"] == "5.19.0"
for name in ("launch.sh", "install.sh", "uninstall.sh"):
    mode = Path("packaging/kpm", name).stat().st_mode
    assert mode & stat.S_IXUSR, name
uninstall = Path("packaging/kpm/uninstall.sh").read_text()
assert "rm -rf" not in uninstall

install = Path("packaging/kpm/install.sh").read_text()
assert "# Name: BookRelay Kindle" in install
assert "# Author: BookRelay contributors" in install
assert "# Icon: data:image/png;base64," in install
icon_match = re.search(r"# Icon: data:image/png;base64,([A-Za-z0-9+/=]+)", install)
assert icon_match, "install hook must contain a base64 PNG icon"
icon = base64.b64decode(icon_match.group(1), validate=True)
assert icon.startswith(b"\x89PNG\r\n\x1a\n"), "scriptlet icon must be a PNG"
assert icon == Path("packaging/kpm/launcher-icon.png").read_bytes(), "scriptlet icon differs from source PNG"
offset = 8
chunks = []
while offset < len(icon):
    assert offset + 12 <= len(icon), "truncated PNG chunk"
    length = struct.unpack_from(">I", icon, offset)[0]
    kind = icon[offset + 4:offset + 8]
    end = offset + 12 + length
    assert end <= len(icon), "truncated PNG data"
    data = icon[offset + 8:offset + 8 + length]
    checksum = struct.unpack_from(">I", icon, offset + 8 + length)[0]
    assert checksum == zlib.crc32(kind + data), f"invalid {kind!r} checksum"
    chunks.append((kind, data))
    offset = end
assert chunks[0][0] == b"IHDR" and chunks[-1][0] == b"IEND"
width, height, depth, color = struct.unpack_from(">IIBB", chunks[0][1])
assert (width, height, depth, color) == (256, 384, 8, 2)
pixels = zlib.decompress(b"".join(data for kind, data in chunks if kind == b"IDAT"))
stride = 1 + width * 3
assert len(pixels) == height * stride, "invalid PNG pixel data"
assert all(pixels[row * stride] <= 4 for row in range(height)), "invalid PNG scanline filter"
print("KPM manifest and hooks are valid")
