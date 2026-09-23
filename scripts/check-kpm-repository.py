#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path


EXPECTED_REPOSITORY_ID = "bookrelay-kindle"
EXPECTED_PACKAGE_ID = "bookrelay-kindle"
EXPECTED_PLATFORM = "kindlehf"
EXPECTED_ASSET = "bookrelay-kindle-kindlehf.kpkg"


def fail(message):
    raise ValueError(message)


def validate(repository, package, release_tag):
    if repository.get("manifest_version") != 2:
        fail("repository manifest_version must be 2")
    if repository.get("id") != EXPECTED_REPOSITORY_ID:
        fail("repository id must be bookrelay-kindle")
    if not isinstance(repository.get("packages"), dict):
        fail("repository packages must be an object")

    package_id = package.get("id")
    package_version = package.get("version")
    package_platforms = package.get("supported_platforms")
    if package.get("manifest_version") != 2:
        fail("package manifest_version must be 2")
    if package_id != EXPECTED_PACKAGE_ID:
        fail("package id must be bookrelay-kindle")
    if package_platforms != [EXPECTED_PLATFORM]:
        fail("package must target kindlehf")

    indexed = repository["packages"].get(EXPECTED_PACKAGE_ID)
    if not isinstance(indexed, dict):
        fail("repository must index bookrelay-kindle")
    artifacts = indexed.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != 1:
        fail("repository package must contain exactly one artifact")
    artifact = artifacts[0]
    if artifact.get("version") != package_version:
        fail("repository artifact version does not match package")
    if artifact.get("dependencies") != []:
        fail("repository artifact dependencies must be empty")
    if artifact.get("supported_platforms") != [EXPECTED_PLATFORM]:
        fail("repository artifact must target kindlehf")

    expected_url = (
        "https://github.com/ishenko/bookrelay-kindle/releases/download/"
        f"{release_tag}/{EXPECTED_ASSET}"
    )
    if artifact.get("url") != expected_url:
        fail("repository artifact URL does not match the release tag and asset")


def main():
    parser = argparse.ArgumentParser(description="Validate the BookRelay KPM repository manifest")
    parser.add_argument("--repository-manifest", type=Path, default=Path("kpm/manifest.json"))
    parser.add_argument("--package-manifest", type=Path, default=Path("packaging/kpm/manifest.json"))
    parser.add_argument("--release-tag", default="v0.1.12")
    args = parser.parse_args()

    try:
        repository = json.loads(args.repository_manifest.read_text(encoding="utf-8"))
        package = json.loads(args.package_manifest.read_text(encoding="utf-8"))
        validate(repository, package, args.release_tag)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"KPM repository validation failed: {error}", file=sys.stderr)
        return 1

    print("KPM repository manifest is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
