"""Recorded spawn facts from real brokers, using only private sessions/PIDs."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())


class RecordedTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="kbrec.")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.rt = self.root / "rt"
        self.rt.mkdir(mode=0o700)
        self.env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": str(self.root), "LANG": "C.UTF-8"}
        self.owned = []
        self.addCleanup(self.stop_owned)

    def cli(self, *args, cwd=None):
        return subprocess.run([CLI, "--runtime-dir", str(self.rt), *args], env=self.env,
                              capture_output=True, stdin=subprocess.DEVNULL, timeout=15, cwd=cwd)

    def document(self, *args):
        result = self.cli(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout.decode("utf-8", errors="replace"))

    def spawn(self, ident="target", extra=(), cwd=None):
        argv = ["/bin/sh", "-c", "exec /bin/sleep 90", *extra]
        result = self.cli("run", "--id", ident, "--", *argv, cwd=cwd or str(self.root))
        self.assertEqual(result.returncode, 0, result.stderr)
        live = self.document("status", ident, "--json")
        self.owned.append((ident, live))
        return live, argv

    def stop_owned(self):
        for ident, live in self.owned:
            try:
                os.kill(live["broker_pid"], signal.SIGCONT)
            except ProcessLookupError:
                pass
            self.cli("kill", ident, "--expect-started", str(live["started_millis"]))
            # These are the exact broker/command PIDs created by this test.
            for pid in (live["child_pid"], live["broker_pid"]):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def stopped_row(self, live):
        os.kill(live["broker_pid"], signal.SIGSTOP)
        result = self.document("--timeout", "0.1", "list", "--json", "--all")
        rows = [row for row in result if row["id"] == live["id"]]
        self.assertEqual(len(rows), 1, result)
        self.assertEqual(rows[0]["reachable"], False)
        self.assertEqual(rows[0]["error"], "timeout")
        self.assertEqual(set(rows[0]), {"id", "reachable", "error", "recorded"})
        return rows[0]

    def metadata(self):
        return self.rt / "sessions" / "target" / "metadata"

    def test_round_trip_and_live_rows_unchanged(self):
        cwd = self.root / "space ' \"\\\n\001 café 🙂"
        cwd.mkdir()
        live, argv = self.spawn(extra=("", "a b", "'quotes\"\\", "new\nline\r\t\001\037\177", "café 🙂"), cwd=str(cwd))
        row = self.document("list", "--json", "--all")[0]
        self.assertNotIn("recorded", row)
        self.assertEqual({key: value for key, value in row.items() if key != "reachable"}, live)
        data = self.metadata().read_bytes()
        fields = dict(line.split("=", 1) for line in data.decode().split("\n") if line)
        self.assertEqual(json.loads(fields["argv_json"]), argv)
        self.assertEqual(json.loads(fields["cwd_json"]), str(cwd))
        self.assertEqual(fields["argv_truncated"], "0")
        self.assertLessEqual(len(data), 4096)
        self.assertLessEqual(len(fields["argv_json"].encode()), 1024)
        self.assertLessEqual(len(fields["cwd_json"].encode()), 512)
        self.assertEqual(self.metadata().stat().st_mode & 0o777, 0o600)
        self.assertEqual({path.name for path in self.metadata().parent.iterdir()}, {"metadata", "control.sock", "journal.bin"})
        recorded = self.stopped_row(live)["recorded"]
        self.assertEqual(recorded, {"argv": argv, "cwd": str(cwd), "started_millis": live["started_millis"], "truncated": False})
        hidden = self.document("--timeout", "0.1", "list", "--json")
        self.assertEqual(hidden, [])
        os.kill(live["broker_pid"], signal.SIGCONT)
        self.assertNotIn("recorded", self.document("status", "target", "--json"))

    def test_bounds_keep_whole_argv_and_a_cwd_character_prefix(self):
        cwd = self.root
        for _ in range(4):
            cwd /= "café" * 30
            cwd.mkdir()
        live, argv = self.spawn(extra=("fits", "x" * 1000, "tail"), cwd=str(cwd))
        recorded = self.stopped_row(live)["recorded"]
        self.assertEqual(recorded["argv"], argv[:4])
        self.assertTrue(recorded["truncated"])
        self.assertTrue(str(cwd).startswith(recorded["cwd"]))
        self.assertLess(len(recorded["cwd"]), len(str(cwd)))
        fields = dict(line.split("=", 1) for line in self.metadata().read_text().splitlines())
        self.assertLessEqual(len(fields["argv_json"].encode()), 1024)
        self.assertLessEqual(len(fields["cwd_json"].encode()), 512)
        self.assertLessEqual(self.metadata().stat().st_size, 4096)

    def test_non_utf8_bytes_are_replaced_and_marked_lossy(self):
        cwd = os.fsencode(self.root) + b"/bad\xff"
        os.mkdir(cwd)
        live, _ = self.spawn(extra=(b"a\xffb\xe2(",), cwd=cwd)
        recorded = self.stopped_row(live)["recorded"]
        self.assertEqual(recorded["argv"][-1], "a\ufffdb\ufffd(")
        self.assertEqual(recorded["cwd"], str(self.root) + "/bad\ufffd")
        self.assertTrue(recorded["truncated"])
        self.metadata().read_bytes().decode("utf-8", errors="strict")

    def test_old_metadata_preserves_the_existing_start_time_and_missing_fields_are_null(self):
        live, _ = self.spawn()
        old = "\n".join(line for line in self.metadata().read_text().splitlines()
                        if not line.startswith(("argv_json=", "cwd_json=", "argv_truncated="))) + "\n"
        self.metadata().write_text(old)
        self.assertEqual(self.stopped_row(live)["recorded"],
                         {"argv": None, "cwd": None, "started_millis": live["started_millis"], "truncated": False})
        self.metadata().write_text(f"broker_pid={live['broker_pid']}\n")
        self.assertEqual(self.stopped_row(live)["recorded"],
                         {"argv": None, "cwd": None, "started_millis": None, "truncated": False})
        self.metadata().unlink()
        self.assertEqual(self.stopped_row(live)["recorded"],
                         {"argv": None, "cwd": None, "started_millis": None, "truncated": False})

    def test_rejected_metadata_is_null_and_never_authorizes_cleanup(self):
        live, _ = self.spawn()
        self.metadata().write_text(f"broker_pid={live['broker_pid']}\nargv_json=[\"x\"],\"injected\":true\ncwd_json=[]\n")
        recorded = self.stopped_row(live)["recorded"]
        self.assertEqual(recorded, {"argv": None, "cwd": None, "started_millis": None, "truncated": True})
        self.metadata().write_bytes(b"x" * 4097)
        self.assertEqual(self.stopped_row(live)["recorded"],
                         {"argv": None, "cwd": None, "started_millis": None, "truncated": False})
        self.assertTrue(self.metadata().parent.is_dir())

    def test_the_complete_4096_byte_record_is_read(self):
        live, _ = self.spawn()
        identity = self.metadata().read_bytes().split(b"argv_json=", 1)[0]
        tail = b'argv_json=["at-the-end"]\ncwd_json="/recorded"\n'
        padding = b"ignored=" + b"x" * (4096 - len(identity) - len(tail) - 9) + b"\n"
        data = identity + padding + tail
        self.assertEqual(len(data), 4096)
        self.metadata().write_bytes(data)
        self.assertEqual(self.stopped_row(live)["recorded"],
                         {"argv": ["at-the-end"], "cwd": "/recorded", "started_millis": live["started_millis"], "truncated": False})


if __name__ == "__main__":
    unittest.main()
