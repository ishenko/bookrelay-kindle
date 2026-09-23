#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "scripts" / "check-kpm-repository.py"


def run_checker(repository, package, tag="v0.1.12"):
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        repository_path = tmp_path / "repository.json"
        package_path = tmp_path / "package.json"
        repository_path.write_text(json.dumps(repository), encoding="utf-8")
        package_path.write_text(json.dumps(package), encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--repository-manifest",
                str(repository_path),
                "--package-manifest",
                str(package_path),
                "--release-tag",
                tag,
            ],
            capture_output=True,
            text=True,
            check=False,
        )


def valid_manifests():
    package = {
        "manifest_version": 2,
        "id": "bookrelay-kindle",
        "version": [0, 1, 12],
        "supported_platforms": ["kindlehf"],
    }
    repository = {
        "manifest_version": 2,
        "id": "bookrelay-kindle",
        "name": "BookRelay Kindle",
        "description": "BookRelay package repository.",
        "packages": {
            "bookrelay-kindle": {
                "name": "BookRelay Kindle",
                "author": "BookRelay contributors",
                "description": "BookRelay Kindle client.",
                "artifacts": [
                    {
                        "url": "https://github.com/ishenko/bookrelay-kindle/releases/download/v0.1.12/bookrelay-kindle-kindlehf.kpkg",
                        "version": [0, 1, 12],
                        "dependencies": [],
                        "supported_platforms": ["kindlehf"],
                    }
                ],
            }
        },
    }
    return repository, package


class RepositoryManifestChecks(unittest.TestCase):
    def test_accepts_release_manifest_matching_package(self):
        repository, package = valid_manifests()
        result = run_checker(repository, package)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_artifact_for_wrong_release(self):
        repository, package = valid_manifests()
        repository["packages"]["bookrelay-kindle"]["artifacts"][0]["url"] = (
            "https://github.com/ishenko/bookrelay-kindle/releases/download/v0.0.9/"
            "bookrelay-kindle-kindlehf.kpkg"
        )
        result = run_checker(repository, package)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release tag", result.stderr)


if __name__ == "__main__":
    unittest.main()
