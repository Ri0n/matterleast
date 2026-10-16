#!/usr/bin/env python3
"""Select old rolling-nightly assets and legacy nightly Releases for safe cleanup.

This command only produces a deletion plan (JSON); the privileged workflow
must execute it after verifying the newly published channel pointer.
"""
import argparse
import json
import re
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

BUILD_ID = re.compile(r"nightly-\d{8}-\d{6}-[0-9a-f]{7,40}\Z")
ASSET = re.compile(
    r"matterleast-\d+\.\d+(?:\.\d+)?-(nightly-\d{8}-\d{6}-[0-9a-f]{7,40})-(.+)\Z"
)
SUFFIXES = frozenset((
    "ubuntu-22.04-amd64.deb", "ubuntu-22.04-amd64-dbgsym.deb",
    "ubuntu-24.04-amd64.deb", "ubuntu-24.04-amd64-dbgsym.deb",
    "ubuntu-26.04-amd64.deb", "ubuntu-26.04-amd64-dbgsym.deb",
    "debian-13-amd64.deb", "debian-13-amd64-dbgsym.deb",
    "fedora-44-x86_64.rpm", "linux-x86_64.AppImage",
    "windows-x64-installer.exe", "windows-x64-portable.zip",
))


def timestamp(build_id: str) -> datetime:
    return datetime.strptime(build_id[8:23], "%Y%m%d-%H%M%S").replace(tzinfo=timezone.utc)


def plan_cleanup(release: dict, pointer: dict, legacy_releases: list,
                 now: datetime, keep_builds: int = 3, days: int = 7) -> dict:
    """Keep the latest three build IDs AND any younger than seven days."""
    if (release.get("tag_name") != "updates-nightly" or release.get("draft") is not False
            or release.get("prerelease") is not True):
        raise ValueError("Refusing to prune an unexpected GitHub Release")
    active = pointer.get("buildId")
    if (pointer.get("schema") != 1 or pointer.get("channel") != "nightly"
            or pointer.get("releaseTag") != "updates-nightly"
            or pointer.get("platform") != "windows-x64"
            or not isinstance(active, str) or not BUILD_ID.fullmatch(active)):
        raise ValueError("Refusing to prune with an invalid active channel pointer")

    by_build = defaultdict(list)
    for asset in release.get("assets", []):
        name = asset.get("name", "")
        match = ASSET.fullmatch(name)
        if match and match.group(2) in SUFFIXES:
            if not isinstance(asset.get("id"), int):
                raise ValueError("Missing GitHub asset ID")
            by_build[match.group(1)].append(asset["id"])
    if active not in by_build:
        raise ValueError("Active build has no assets in the rolling release")

    retained = set(sorted(by_build.keys(), reverse=True)[:keep_builds]) | {active}
    cutoff = now - timedelta(days=days)
    deleted_assets = []
    for build_id, asset_ids in by_build.items():
        if build_id not in retained and timestamp(build_id) < cutoff:
            deleted_assets.extend(asset_ids)

    legacy_tags = []
    for old in legacy_releases:
        tag = old.get("tag_name", "")
        if not isinstance(tag, str) or not BUILD_ID.fullmatch(tag):
            continue
        if old.get("draft") is not False or old.get("prerelease") is not True:
            continue
        published = old.get("published_at")
        if not isinstance(published, str):
            continue
        try:
            published_time = datetime.fromisoformat(published.replace("Z", "+00:00"))
        except ValueError:
            continue
        if published_time < cutoff and tag != active:
            legacy_tags.append(tag)
    return {
        "asset_ids": sorted(deleted_assets),
        "legacy_release_tags": sorted(set(legacy_tags)),
        "retained_build_ids": sorted(retained, reverse=True),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--release", required=True, type=Path)
    parser.add_argument("--pointer", required=True, type=Path)
    parser.add_argument("--legacy", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    legacy = [json.loads(line) for line in options.legacy.read_text().splitlines() if line.strip()]
    result = plan_cleanup(
        json.loads(options.release.read_text()),
        json.loads(options.pointer.read_text()),
        legacy, datetime.now(timezone.utc),
    )
    options.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
