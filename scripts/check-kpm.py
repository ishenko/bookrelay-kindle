#!/usr/bin/env python3
import json
import stat
from pathlib import Path

manifest = json.loads(Path("packaging/kpm/manifest.json").read_text())
assert manifest["manifest_version"] == 3
assert manifest["id"] == "bookrelay-kindle"
assert manifest["version"] == [0, 1, 0]
assert manifest["supported_platforms"] == ["kindlehf"]
assert manifest["bookrelay_min_firmware"] == "5.19.0"
for name in ("launch.sh", "install.sh", "uninstall.sh"):
    mode = Path("packaging/kpm", name).stat().st_mode
    assert mode & stat.S_IXUSR, name
uninstall = Path("packaging/kpm/uninstall.sh").read_text()
assert "rm -rf" not in uninstall
print("KPM manifest and hooks are valid")
