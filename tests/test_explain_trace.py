"""Tests for scripts/explain_trace.py against a stub HTTP server; no network.

Set STATEGUARD_BUILD_DIR to the directory holding stateguard-check and
stateguard-timeline (CTest does this).
"""

import glob
import http.server
import json
import os
import subprocess
import sys
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "explain_trace.py")
FIXTURES = os.path.join(ROOT, "tests", "fixtures")
BUILD_DIR = os.environ.get("STATEGUARD_BUILD_DIR", os.path.join(ROOT, "build"))
CANNED = "The reclaim happened before copy-1 completed."


class StubHandler(http.server.BaseHTTPRequestHandler):
    status = 200
    requests = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        StubHandler.requests.append((self.path, self.headers.get("Authorization"), body))
        if StubHandler.status != 200:
            self.send_response(StubHandler.status)
            self.end_headers()
            self.wfile.write(b'{"error": "nope"}')
            return
        reply = {"choices": [{"message": {"role": "assistant", "content": CANNED}}]}
        data = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def run(*args, env_key="test-key"):
    env = dict(os.environ)
    env.pop("SG_TEST_KEY", None)
    if env_key is not None:
        env["SG_TEST_KEY"] = env_key
    return subprocess.run(
        [sys.executable, SCRIPT, "--build-dir", BUILD_DIR, "--api-key-env", "SG_TEST_KEY", *args],
        capture_output=True, text=True, env=env)


class ExplainTraceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), StubHandler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base_url = f"http://127.0.0.1:{cls.server.server_address[1]}/v1"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        StubHandler.status = 200
        StubHandler.requests = []

    def test_stub_completion(self):
        proc = run(os.path.join(FIXTURES, "premature_reclaim.sgtrace"), "--base-url", self.base_url)
        self.assertEqual(proc.returncode, 1, proc.stderr)
        lines = proc.stdout.splitlines()
        self.assertEqual(lines[0], "violation")
        self.assertIn("premature_reclaim", lines[1])
        self.assertIn("LLM-assisted explanation (not evidence):", lines)
        self.assertEqual(lines[-1], CANNED)
        path, auth, body = StubHandler.requests[0]
        self.assertEqual(path, "/v1/chat/completions")
        self.assertEqual(auth, "Bearer test-key")
        self.assertEqual(body["model"], "muse-spark-1.3-contributor")
        self.assertEqual([m["role"] for m in body["messages"]], ["system", "user"])
        self.assertIn("reclaim|buffer-1|1", body["messages"][1]["content"])

    def test_timeline_included_for_timed_trace(self):
        proc = run(os.path.join(FIXTURES, "cancellation_timeline.sgtrace"), "--base-url", self.base_url)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        user = StubHandler.requests[0][2]["messages"][1]["content"]
        self.assertIn("stateguard-timeline --json", user)

    def test_dry_run_every_fixture(self):
        for trace in sorted(glob.glob(os.path.join(FIXTURES, "*.sgtrace"))):
            with self.subTest(trace=os.path.basename(trace)):
                proc = run(trace, "--dry-run", env_key="secret-value")
                self.assertIn(proc.returncode, (0, 1, 2), proc.stderr)
                self.assertNotIn("secret-value", proc.stdout)
                request = json.loads(proc.stdout.split("\n", 2)[2])
                self.assertEqual(request["headers"]["Authorization"], "Bearer <redacted>")
                self.assertTrue(request["url"].endswith("/chat/completions"))
        self.assertEqual(StubHandler.requests, [])

    def test_oversize_refused(self):
        proc = run(os.path.join(FIXTURES, "premature_reclaim.sgtrace"),
                   "--max-trace-bytes", "10", "--base-url", self.base_url)
        self.assertEqual(proc.returncode, 3)
        self.assertIn("max-trace-bytes", proc.stderr)
        self.assertEqual(StubHandler.requests, [])

    def test_missing_key(self):
        proc = run(os.path.join(FIXTURES, "premature_reclaim.sgtrace"),
                   "--base-url", self.base_url, env_key=None)
        self.assertEqual(proc.returncode, 4)
        self.assertIn("SG_TEST_KEY", proc.stderr)
        self.assertTrue(proc.stdout.startswith("violation\n"))

    def test_bad_status(self):
        StubHandler.status = 500
        proc = run(os.path.join(FIXTURES, "premature_reclaim.sgtrace"), "--base-url", self.base_url)
        self.assertEqual(proc.returncode, 4)
        self.assertIn("HTTP 500", proc.stderr)
        self.assertTrue(proc.stdout.startswith("violation\n"))
        self.assertNotIn("LLM-assisted", proc.stdout)

    def test_unreachable_endpoint(self):
        proc = run(os.path.join(FIXTURES, "premature_reclaim.sgtrace"),
                   "--base-url", "http://127.0.0.1:9/v1", "--timeout", "5")
        self.assertEqual(proc.returncode, 4)
        self.assertTrue(proc.stdout.startswith("violation\n"))


if __name__ == "__main__":
    unittest.main()
