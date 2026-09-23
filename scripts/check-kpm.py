#!/usr/bin/env python3
import json
import base64
import re
import stat
from pathlib import Path

manifest = json.loads(Path("packaging/kpm/manifest.json").read_text())
assert manifest["manifest_version"] == 2
assert manifest["id"] == "bookrelay-kindle"
assert manifest["version"] == [0, 1, 7]
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
print("KPM manifest and hooks are valid")
