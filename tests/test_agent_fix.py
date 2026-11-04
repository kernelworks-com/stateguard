"""Tests for scripts/agent_fix.py with the fake model; no network.

Set STATEGUARD_BUILD_DIR to a configured and built build directory (CTest
does this). The loop compiles scratch copies under STATEGUARD_BUILD_DIR/agent-fix.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "agent_fix.py")
DIFFS = os.path.join(ROOT, "tests", "agent_fix")
BUILD_DIR = os.environ.get("STATEGUARD_BUILD_DIR", os.path.join(ROOT, "build"))


def run(*args):
    env = dict(os.environ)
    env.pop("META_API_KEY", None)
    return subprocess.run([sys.executable, SCRIPT, "--build-dir", BUILD_DIR, *args],
                          capture_output=True, text=True, env=env)


class AgentFixTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.transcript = os.path.join(self.tmp.name, "run.json")

    def tearDown(self):
        self.tmp.cleanup()

    def fake(self, bug, diff, attempts):
        proc = run("--bug", bug, "--model", "fake:" + os.path.join(DIFFS, diff),
                   "--max-attempts", str(attempts), "--transcript", self.transcript)
        with open(self.transcript, encoding="utf-8") as handle:
            return proc, json.load(handle)

    def test_known_good_diff_is_accepted(self):
        proc, transcript = self.fake("premature-reclaim", "premature_reclaim_good.diff", 3)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        result = transcript["result"]
        self.assertTrue(result["accepted"])
        self.assertEqual(result["attempts"], 1)
        self.assertEqual(transcript["planted"]["explore"]["passed"], 0)
        attempt = transcript["attempts"][0]
        self.assertEqual(attempt["stage"], "accepted")
        explore = attempt["explore"]
        self.assertGreater(explore["schedules"], 1000)
        self.assertEqual(explore["passed"], explore["schedules"])
        self.assertEqual(explore["clean"], explore["schedules"])
        self.assertEqual(explore["corrupted_bytes"], 0)
        for replay in attempt["replay"].values():
            self.assertEqual(replay["passed"], 1)
        self.assertEqual(result["cost_usd"], 0)
        # The tracked harness source is never edited in place.
        with open(os.path.join(ROOT, "harness", "async_copy.cpp"), encoding="utf-8") as handle:
            self.assertIn("ReclaimPolicy::Immediate || record.holds == 0", handle.read())

        replay = subprocess.run([sys.executable, SCRIPT, "--replay", self.transcript,
                                 "--pace", "0"], capture_output=True, text=True)
        self.assertEqual(replay.returncode, 0, replay.stderr)
        self.assertIn("== attempt 1", replay.stdout)
        self.assertIn("accepted after 1 attempt(s)", replay.stdout)

    def test_known_bad_diff_is_rejected(self):
        proc, transcript = self.fake("reclaim-on-timeout", "reclaim_on_timeout_bad.diff", 2)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertFalse(transcript["result"]["accepted"])
        self.assertEqual(len(transcript["attempts"]), 2)
        for attempt in transcript["attempts"]:
            self.assertEqual(attempt["stage"], "explore")
            self.assertLess(attempt["explore"]["passed"], attempt["explore"]["schedules"])
            self.assertEqual(attempt["check"]["status"], "violation")
        # The second request carries the first attempt's diagnostics.
        feedback = transcript["attempts"][1]["messages"][-1]["content"]
        self.assertIn("schedules still fail", feedback)
        self.assertIn("stateguard-reduce witness", feedback)

    def test_edit_outside_the_host_is_rejected_before_building(self):
        proc, transcript = self.fake("premature-reclaim", "outside_region.diff", 1)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        attempt = transcript["attempts"][0]
        self.assertEqual(attempt["stage"], "patch")
        self.assertIn("outside the CacheHost member functions", attempt["reason"])

    def test_dry_run_prints_the_request_without_a_key(self):
        proc = run("--bug", "tag-only-attribution", "--dry-run")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        request = json.loads(proc.stdout[proc.stdout.index("{"):])
        self.assertEqual(request["headers"]["Authorization"], "Bearer <redacted>")
        messages = request["body"]["messages"]
        self.assertIn("unified diff", messages[0]["content"])
        self.assertIn("CacheHost::attribute", messages[1]["content"])
        self.assertIn("stateguard-check --json", messages[1]["content"])

    def test_missing_key_is_a_tooling_error(self):
        proc = run("--bug", "premature-reclaim", "--max-attempts", "1")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("META_API_KEY", proc.stderr)

    def test_list_bugs(self):
        proc = run("--list-bugs")
        self.assertEqual(proc.returncode, 0)
        for name in ("premature-reclaim", "counted-redelivery", "failed-load-publish",
                     "tag-only-attribution", "reclaim-on-timeout"):
            self.assertIn(name, proc.stdout)


if __name__ == "__main__":
    unittest.main()
