#!/usr/bin/env python3
"""Create checked Windows update pointers from final GitHub Release package bytes."""
import argparse
import hashlib
import json
import re
from pathlib import Path

REPO = "Ri0n/matterleast"
STABLE_ASSET = re.compile(r"matterleast-(\d+\.\d+(?:\.\d+)?)-windows-x64-portable\.zip\Z")
VERSION = r"\d+\.\d+(?:\.\d+)?"
STABLE = re.compile(r"v(" + VERSION + r")\Z")
NIGHTLY = re.compile(r"nightly-\d{8}-\d{6}-[0-9a-f]{7,40}\Z")
NIGHTLY_ASSET = re.compile(
    r"matterleast-(" + VERSION + r")-(nightly-\d{8}-\d{6}-[0-9a-f]{7,40})"
    r"-windows-x64-portable\.zip\Z"
)


def make_manifest(channel: str, identity: str, file: Path) -> dict:
    if channel == "stable":
        match = STABLE_ASSET.fullmatch(file.name)
        valid = STABLE.fullmatch(identity)
        if not match or not valid or valid.group(1) != match.group(1):
            raise ValueError("Stable release tag and Windows asset version must match")
        version, release_tag = match.group(1), identity
        build_id = None
    elif channel == "nightly":
        match = NIGHTLY_ASSET.fullmatch(file.name)
        if not match or not NIGHTLY.fullmatch(identity) or match.group(2) != identity:
            raise ValueError("Nightly package name must include the exact immutable build ID")
        version, release_tag, build_id = match.group(1), "updates-nightly", identity
    else:
        raise ValueError("Invalid update channel")

    if file.is_symlink() or not file.is_file() or file.stat().st_size == 0:
        raise ValueError("Asset must be a nonempty regular file")
    digest = hashlib.sha256()
    with file.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)

    result = {
        "schema": 1,
        "channel": channel,
        "platform": "windows-x64",
        "version": version,
        "releaseTag": release_tag,
        "minimumLauncherProtocol": 1,
        "package": {
            "format": "portable-zip",
            "url": f"https://github.com/{REPO}/releases/download/{release_tag}/{file.name}",
            "size": file.stat().st_size,
            "sha256": digest.hexdigest(),
        },
    }
    if build_id is not None:
        result["buildId"] = build_id
    return result


def validate_promotion(previous: dict, proposed: dict, allow_rollback: bool = False) -> None:
    """Fail closed on malformed prior pointers or unintentional build downgrades."""
    if (previous.get("schema") != 1
            or previous.get("channel") != proposed["channel"]
            or previous.get("platform") != "windows-x64"):
        raise ValueError("Previous pointer has an unexpected schema/channel/platform")
    if proposed["channel"] == "stable":
        old, new = previous.get("releaseTag"), proposed["releaseTag"]
        if not isinstance(old, str) or not STABLE.fullmatch(old):
            raise ValueError("Previous stable release tag is invalid")
        old_key = tuple(int(x) for x in old[1:].split("."))
        new_key = tuple(int(x) for x in new[1:].split("."))
        old_key += (0,) * (3 - len(old_key))
        new_key += (0,) * (3 - len(new_key))
        downgrade = new_key < old_key or (new_key == old_key and new != old)
    else:
        # One-time migration from the first legacy per-build nightly Release:
        # that schema-1 manifest used releaseTag as the build identity.
        if previous.get("releaseTag") == "updates-nightly":
            old = previous.get("buildId")
        elif "buildId" not in previous:
            old = previous.get("releaseTag")
        else:
            raise ValueError("Previous nightly releaseTag is invalid")
        new = proposed["buildId"]
        if not isinstance(old, str) or not NIGHTLY.fullmatch(old):
            raise ValueError("Previous nightly build identity is invalid")
        downgrade = new < old
    if downgrade and not allow_rollback:
        raise ValueError("Refusing to move an update channel backwards")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", required=True, choices=("stable", "nightly"))
    parser.add_argument("--release-tag", help="Stable immutable tag, e.g. v1.9")
    parser.add_argument("--build-id", help="Nightly timestamped build identity")
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--allow-rollback", action="store_true")
    options = parser.parse_args()
    if options.channel == "stable":
        if not options.release_tag or options.build_id:
            parser.error("Stable needs --release-tag and must not use --build-id")
        identity = options.release_tag
    else:
        if not options.build_id or options.release_tag:
            parser.error("Nightly needs --build-id and must not use --release-tag")
        identity = options.build_id
    manifest = make_manifest(options.channel, identity, options.asset)
    if options.previous:
        validate_promotion(json.loads(options.previous.read_text(encoding="utf-8")),
                           manifest, options.allow_rollback)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
