#!/usr/bin/env python3
"""Package the LevelUp-NX release asset.

The public release carries exactly **one** asset, and its filename carries the
version: `LevelUp-NX-vX.Y.Z.nro`.

This fork used to inherit the upstream Moonlight-Switch "All Builds" packaging
set (Android APK/IPA, Linux AppImage/deb/rpm/tar.gz for two arches, macOS zip,
PSVita vpk, SteamOS tarballs, Switch2 zips, Windows zips, plus a 100 MB debug
ELF). Those binaries are not produced from this tree — they only bloated every
release page by ~1.2 GB and confused users. Do not reintroduce them: the
collector below only ever emits the Switch client.

Usage:
    python scripts/ci/collect_release.py v0.2.0 build/switch dist
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

ASSET_BASENAME = "LevelUp-NX"
TAG_PATTERN = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")


def asset_name(tag: str) -> str:
    """Public asset filename — the version must stay visible in the download name."""
    return f"{ASSET_BASENAME}-{tag}.nro"


def collect(tag: str, artifacts: Path, output: Path) -> list[Path]:
    """Validate the tag/artifact set, then write the single release file."""
    if not TAG_PATTERN.fullmatch(tag):
        raise ValueError(f"Expected a vX.X.X release tag, got {tag!r}")

    artifacts = Path(artifacts)
    if not artifacts.exists():
        raise ValueError(f"Artifacts path not found: {artifacts}")

    matches = [
        path
        for path in artifacts.rglob("*.nro")
        if path.is_file() and path.name.startswith(ASSET_BASENAME)
    ]
    if not matches:
        raise ValueError(f"Missing {ASSET_BASENAME}.nro under {artifacts}")
    if len(matches) > 1:
        raise ValueError(
            f"Ambiguous {ASSET_BASENAME}.nro candidates: {sorted(str(p) for p in matches)}"
        )

    output = Path(output)
    destination = output / asset_name(tag)
    if destination.exists():
        raise ValueError(f"Release asset already exists: {destination}")

    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(matches[0], destination)
    return [destination]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        for asset in collect(args.tag, args.artifacts, args.output):
            print(asset)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
