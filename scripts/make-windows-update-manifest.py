#!/usr/bin/env python3
"""Build a Windows update manifest from the exact immutable Release ZIP bytes."""
import argparse
import hashlib
import json
import re
from pathlib import Path

REPO = "Ri0n/matterleast"
ASSET = re.compile(r"matterleast-(\d+\.\d+(?:\.\d+)?)-windows-x64-portable\.zip\Z")
STABLE = re.compile(r"v(\d+\.\d+(?:\.\d+)?)\Z")
NIGHTLY = re.compile(r"nightly-\d{8}-\d{6}-[0-9a-f]{7,40}\Z")


def make_manifest(channel: str, tag: str, file: Path) -> dict:
    if channel not in ("stable", "nightly"):
        raise ValueError("Invalid update channel")
    match = ASSET.fullmatch(file.name)
    if not match:
        raise ValueError("Invalid Windows asset name")
    version = match.group(1)
    if channel == "stable":
        valid = STABLE.fullmatch(tag)
        if not valid or valid.group(1) != version:
            raise ValueError("Stable tag must match the asset version")
    elif not NIGHTLY.fullmatch(tag):
        raise ValueError("Nightly tag must be immutable and timestamped")
    if file.is_symlink() or not file.is_file() or file.stat().st_size == 0:
        raise ValueError("Asset must be a nonempty regular file")
    digest = hashlib.sha256()
    with file.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return {
        "schema": 1,
        "channel": channel,
        "platform": "windows-x64",
        "version": version,
        "releaseTag": tag,
        "minimumLauncherProtocol": 1,
        "package": {
            "format": "portable-zip",
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{file.name}",
            "size": file.stat().st_size,
            "sha256": digest.hexdigest(),
        },
    }


def validate_promotion(previous: dict, proposed: dict, allow_rollback: bool = False) -> None:
    """Reject accidental channel rollback when replaying old release workflows."""
    if previous.get("schema") != 1 or previous.get("channel") != proposed["channel"]:
        raise ValueError("Existing channel pointer has an unexpected schema or channel")
    old, new = previous.get("releaseTag"), proposed["releaseTag"]
    if not isinstance(old, str):
        raise ValueError("Previous channel releaseTag is invalid")
    if proposed["channel"] == "stable":
        matched = STABLE.fullmatch(old)
        if not matched:
            raise ValueError("Previous stable channel releaseTag is invalid")
        old_key = tuple(int(v) for v in matched.group(1).split("."))
        new_key = tuple(int(v) for v in proposed["version"].split("."))
        old_key += (0,) * (3 - len(old_key))
        new_key += (0,) * (3 - len(new_key))
        downgrade = new_key < old_key or (new_key == old_key and new != old)
    else:
        if not NIGHTLY.fullmatch(old):
            raise ValueError("Previous nightly channel releaseTag is invalid")
        downgrade = new < old
    if downgrade and not allow_rollback:
        raise ValueError("Refusing to move an update channel backwards")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", required=True, choices=("stable", "nightly"))
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--allow-rollback", action="store_true")
    options = parser.parse_args()
    manifest = make_manifest(options.channel, options.release_tag, options.asset)
    if options.previous:
        validate_promotion(
            json.loads(options.previous.read_text(encoding="utf-8")),
            manifest, options.allow_rollback)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
