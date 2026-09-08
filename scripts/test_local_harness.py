"""Lifecycle smoke tests using stand-in executables, not an emulator/server."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


HARNESS = Path(__file__).with_name("local_test.py")


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fzvs harness ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fake = self.root / "stand in"
        self.fake.write_text(f"#!{sys.executable}\n" + '''
import os, pathlib, sys, time
args = sys.argv[1:]
print("stand-in running", flush=True)
if "--ready-file" in args:
    if os.environ.get("FZVS_TEST_MODE") == "server_exit":
        sys.exit(7)
    if os.environ.get("FZVS_TEST_MODE") != "no_ready":
        pathlib.Path(args[args.index("--ready-file") + 1]).touch()
else:
    profile = pathlib.Path(args[args.index("--fzvs-profile") + 1])
    assert profile == pathlib.Path.cwd()
    (profile / "observed.json").write_text(__import__("json").dumps(args))
    if os.environ.get("FZVS_TEST_MODE") == "client_exit":
        sys.exit(8)
while True:
    time.sleep(0.1)
''')
        self.fake.chmod(0o755)
        self.rom = self.root / "test rom.sfc"
        self.rom.touch()
        self.runs = self.root / "runs"
        self.command = [sys.executable, str(HARNESS), "--client", str(self.fake),
                        "--server", str(self.fake), "--rom", str(self.rom),
                        "--run-root", str(self.runs), "--startup-timeout", "2"]

    def result(self):
        return json.loads(next(self.runs.glob("*/result.json")).read_text())

    def assert_stopped(self, result):
        for child in result["processes"]:
            with self.assertRaises(ProcessLookupError):
                os.kill(child["pid"], 0)

    def run_setup(self, mode="", *extra):
        env = dict(os.environ, FZVS_TEST_MODE=mode)
        return subprocess.run(self.command + list(extra), env=env, capture_output=True,
                              text=True, timeout=10)

    def test_four_clients_isolated_and_duration_cleanup(self):
        run = self.run_setup("", "--players", "4", "--duration", "0.5")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.result()
        self.assertEqual(result["reason"], "duration_elapsed")
        self.assertEqual(len(result["processes"]), 5)
        session = next(self.runs.iterdir())
        plan = json.loads((session / "launch.json").read_text())
        for index, entry in enumerate(plan["processes"][1:]):
            args = json.loads((Path(entry["cwd"]) / "observed.json").read_text())
            self.assertEqual("--fzvs-mute" in args, index != 0)
            self.assertIn("stand-in running", Path(entry["log"]).read_text())
        self.assert_stopped(result)

    def test_server_exit_does_not_launch_clients(self):
        run = self.run_setup("server_exit")
        self.assertEqual(run.returncode, 1)
        self.assertEqual(len(self.result()["processes"]), 1)
        self.assert_stopped(self.result())

    def test_readiness_timeout_cleans_server(self):
        run = self.run_setup("no_ready", "--startup-timeout", "0.2")
        self.assertEqual(run.returncode, 1)
        self.assertIn("timed out", run.stderr)
        self.assert_stopped(self.result())

    def test_client_failure_cleans_every_process(self):
        run = self.run_setup("client_exit")
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.result()["process_exit_code"], 8)
        self.assert_stopped(self.result())

    def test_signal_stops_whole_setup(self):
        process = subprocess.Popen(self.command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not list(self.runs.glob("*/client-2/observed.json")):
                if process.poll() is not None or time.monotonic() > deadline:
                    self.fail("stand-in clients did not start")
                time.sleep(0.02)
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130)
            self.assert_stopped(self.result())
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=5)

    def test_dry_run_and_invalid_inputs_write_nothing(self):
        run = self.run_setup("", "--dry-run", "--players", "4")
        self.assertEqual(run.returncode, 0)
        self.assertIn("client-4", run.stdout)
        self.assertFalse(self.runs.exists())
        run = self.run_setup("", "--input", "keyboard", "--input", "keyboard")
        self.assertEqual(run.returncode, 2)
        self.assertFalse(self.runs.exists())


if __name__ == "__main__":
    unittest.main()
