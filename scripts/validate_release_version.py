#!/usr/bin/env python3

import argparse
import json
import os
import re
from pathlib import Path


SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
ROOT = Path(__file__).resolve().parents[1]


def properties_version() -> str:
    for line in (ROOT / "library.properties").read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            return line.removeprefix("version=").strip()
    raise SystemExit("library.properties does not contain version=")


def release_tag(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        return os.environ.get("GITHUB_REF_NAME")
    ref = os.environ.get("GITHUB_REF", "")
    return ref.removeprefix("refs/tags/") if ref.startswith("refs/tags/") else None


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", help="release tag to compare with the manifests")
    args = parser.parse_args()

    properties = properties_version()
    manifest = json.loads((ROOT / "library.json").read_text(encoding="utf-8"))["version"]
    if properties != manifest:
        raise SystemExit(
            f"version mismatch: library.properties={properties}, library.json={manifest}"
        )
    if not SEMVER.fullmatch(properties):
        raise SystemExit(f"manifest version is not stable semantic version: {properties}")

    tag = release_tag(args.tag)
    if tag is not None and tag != f"v{properties}":
        raise SystemExit(f"tag {tag} does not match manifest version v{properties}")

    print(f"validated Link version {properties}" + (f" for tag {tag}" if tag else ""))


if __name__ == "__main__":
    main()
