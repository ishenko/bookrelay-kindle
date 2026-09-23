#!/usr/bin/env python3
import argparse
import json
import os
import tarfile
from pathlib import Path


PACKAGE_FILES = ["manifest.json", "install.sh", "launch.sh", "uninstall.sh"]


def main():
    parser = argparse.ArgumentParser(description="Build a BookRelay Kindle KPM .kpkg archive")
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest_path = Path("packaging/kpm/manifest.json")
    manifest = json.loads(manifest_path.read_text())
    if manifest["manifest_version"] != 2:
        raise SystemExit("unsupported KPM manifest version")
    if manifest["supported_platforms"] != ["kindlehf"]:
        raise SystemExit("package must target kindlehf")
    if manifest["bookrelay_min_firmware"] != "5.19.0":
        raise SystemExit("unexpected minimum firmware")
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        raise SystemExit("client binary must exist and be executable")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w:gz") as archive:
        for name in PACKAGE_FILES:
            archive.add(Path("packaging/kpm") / name, arcname=name)
        archive.add(args.binary, arcname="bookrelay-kindle")
        for cover in sorted(Path("client/share/covers").glob("*.jpg")):
            archive.add(cover, arcname=f"share/covers/{cover.name}")
        for icon in sorted(Path("client/share/icons").glob("*.png")):
            archive.add(icon, arcname=f"share/icons/{icon.name}")


if __name__ == "__main__":
    main()
