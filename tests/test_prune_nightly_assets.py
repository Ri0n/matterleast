"""Safety tests for automatic rolling-nightly asset and legacy release pruning."""
import importlib.util
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

FILE = Path(__file__).resolve().parents[1] / "scripts" / "prune-nightly-assets.py"
SPEC = importlib.util.spec_from_file_location("prune", FILE)
PRUNE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRUNE)

NOW = datetime(2026, 11, 1, tzinfo=timezone.utc)
IDS = [
    "nightly-20261010-010000-aaa1234",
    "nightly-20261011-010000-aaa1234",
    "nightly-20261012-010000-aaa1234",
    "nightly-20261013-010000-aaa1234",
    "nightly-20261031-010000-aaa1234",
]
ACTIVE = IDS[-1]


def release():
    assets = []
    for i, build in enumerate(IDS):
        assets.append({
            "id": 100 + i,
            "name": f"matterleast-1.9-{build}-windows-x64-portable.zip",
        })
    assets += [
        {"id": 900, "name": "windows-x64.json"},
        {"id": 901, "name": "matterleast-1.9-windows-x64-portable.zip"},
        {"id": 902, "name": "random-file.zip"},
        {"id": 903, "name": "matterleast-1.9-nightly-20261010-010000-aaa1234-unknown.txt"},
    ]
    return {"tag_name": "updates-nightly", "draft": False, "prerelease": True,
            "assets": assets}


def pointer(build=ACTIVE):
    return {"schema": 1, "platform": "windows-x64", "channel": "nightly",
            "releaseTag": "updates-nightly", "buildId": build}


class PruningTests(unittest.TestCase):
    def test_prunes_old_but_keeps_last_three_and_unmanaged(self):
        plan = PRUNE.plan_cleanup(release(), pointer(), [], NOW)
        self.assertEqual(plan["asset_ids"], [100, 101])
        self.assertEqual(plan["retained_build_ids"], list(reversed(IDS[-3:])))

    def test_keep_older_current_build(self):
        plan = PRUNE.plan_cleanup(release(), pointer(IDS[0]), [], NOW)
        self.assertNotIn(100, plan["asset_ids"])

    def test_keep_any_recent_build_even_if_fourth_newest(self):
        data = release()
        extra = "nightly-20261030-010000-aaa1234"
        data["assets"].append({"id": 250, "name": f"matterleast-1.9-{extra}-windows-x64-portable.zip"})
        plan = PRUNE.plan_cleanup(data, pointer(), [], NOW)
        self.assertNotIn(250, plan["asset_ids"])

    def test_deletes_only_expired_legacy_prereleases(self):
        old = [
            {"tag_name": "nightly-20261010-010000-aaa1234", "draft": False,
             "prerelease": True, "published_at": "2026-10-10T01:00:00Z"},
            {"tag_name": "nightly-20261031-010000-aaa1234", "draft": False,
             "prerelease": True, "published_at": "2026-10-31T01:00:00Z"},
            {"tag_name": "v1.9", "draft": False, "prerelease": False,
             "published_at": "2026-10-01T00:00:00Z"},
        ]
        plan = PRUNE.plan_cleanup(release(), pointer(), old, NOW)
        self.assertEqual(plan["legacy_release_tags"], ["nightly-20261010-010000-aaa1234"])

    def test_wrong_pointer_refuses_all_deletions(self):
        for problem in [
            dict(pointer(), releaseTag="v1.9"),
            dict(pointer(), channel="stable"),
            dict(pointer(), buildId="garbage"),
            dict(pointer(), buildId="nightly-20261101-010000-aaa1234"),
        ]:
            with self.subTest(problem=problem), self.assertRaises(ValueError):
                PRUNE.plan_cleanup(release(), problem, [], NOW)
        with self.assertRaises(ValueError):
            PRUNE.plan_cleanup(dict(release(), tag_name="v1.9"), pointer(), [], NOW)


if __name__ == "__main__":
    unittest.main()
