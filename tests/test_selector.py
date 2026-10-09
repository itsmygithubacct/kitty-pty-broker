"""Deterministic real-broker selector regressions; private disposable runtimes only.
Run: python3 tests/test_selector.py build/kitty-pty-broker [OLD_BINARY]
"""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

CLI = str(Path(__file__).resolve().parents[1] / "build/kitty-pty-broker")
OLD = None
if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        CLI = str(Path(sys.argv.pop(1)).resolve())
    if len(sys.argv) > 1 and not sys.argv[1].startswith(("-", "RealBroker.")):
        OLD = str(Path(sys.argv.pop(1)).resolve())
HEADER = struct.Struct(">IHHI")


def frame(sock, kind, payload=b""):
    sock.sendall(HEADER.pack(0x4b504231, 1, kind, len(payload)) + payload)


def receive(sock):
    def read(n):
        out = b""
        while len(out) < n:
            data = sock.recv(n - len(out))
            if not data:
                raise EOFError()
            out += data
        return out
    magic, version, kind, n = HEADER.unpack(read(12))
    assert magic == 0x4b504231 and version == 1 and n < 65536
    return kind, read(n)


def ticks(pid):
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
    except (FileNotFoundError, IndexError):
        return None


def restore_proxy_files(path, moved, metadata, original):
    """Restore a surviving fixture; an ended session has removed its directory."""
    try:
        path.unlink(missing_ok=True)
        moved.rename(path)
        metadata.write_bytes(original)
    except FileNotFoundError:
        try:
            path.parent.stat()
        except FileNotFoundError:
            return
        raise


class RealBroker(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sel-")
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "rt"
        self.runtime.mkdir(mode=0o700)
        self.started = {}
        self.sockets = []
        self.env = {**os.environ, "KITTY_PTY_BROKER_SESSION": "caller_unrelated"}

    def call(self, *args, cli=CLI, timeout="0.3", env=None):
        return subprocess.run([cli, "--runtime-dir", str(self.runtime), "--timeout", timeout, *args],
                              env=self.env if env is None else env, capture_output=True, text=True, timeout=8)

    def start(self, ident, command=None, cli=CLI):
        launcher = subprocess.Popen([cli, "--runtime-dir", str(self.runtime), "run", "--id", ident, "--",
                                     *(command or ["/bin/sleep", "60"])], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=self.env)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            result = self.call("status", ident, "--json")
            if result.returncode == 0:
                status = json.loads(result.stdout)
                self.started[ident] = status
                break
            time.sleep(0.01)
        else:
            launcher.kill(); launcher.wait()
            self.fail("session did not start: " + launcher.stderr.read().decode())
        launcher.wait(timeout=3)
        launcher.stderr.close()
        return status

    def connect(self, ident):
        connection = socket.socket(socket.AF_UNIX)
        connection.settimeout(3)
        connection.connect(str(self.runtime / "sessions" / ident / "control.sock"))
        self.sockets.append(connection)
        return connection

    def attach(self, ident):
        connection = self.connect(ident)
        frame(connection, 1, struct.pack(">HHHH", 24, 80, 0, 0))
        for _ in range(8):
            kind, _ = receive(connection)
            if kind == 8:
                break
        else:
            self.fail("no attach reply")
        return connection

    def resolve(self, selector):
        result = self.call("resolve", *self.args(selector), "--json")
        return json.loads(result.stdout)

    @staticmethod
    def args(selector):
        if "id" in selector:
            result = ["--id", selector["id"]]
        elif "id_prefix" in selector:
            result = ["--id-prefix", selector["id_prefix"]]
        else:
            result = [part for word in selector["startup_words"] for part in ("--command-word", word)]
        if "attached" in selector:
            result += ["--attached" if selector["attached"] else "--detached"]
        return result

    def end(self, selector, ident, started=None, env=None):
        result = self.call("end", *self.args(selector), "--json", "--resolved-id", ident, "--expect-started",
                           str(started or self.started[ident]["started_millis"]), env=env)
        return result.returncode, json.loads(result.stdout)

    def alive(self, ident):
        result = self.call("status", ident, "--json")
        return result.returncode == 0 and json.loads(result.stdout)["started_millis"] == self.started[ident]["started_millis"]

    def tearDown(self):
        for connection in self.sockets:
            connection.close()
        for ident, status in self.started.items():
            pid = status["broker_pid"]
            if ticks(pid) == status["start_ticks"]:
                os.kill(pid, signal.SIGCONT)
            self.call("kill", ident, "--expect-started", str(status["started_millis"]))
        self.temporary.cleanup()

    def test_prefix_count_and_exact_positive_vary_length_and_order(self):
        for prefix, suffixes in (("Z", ["_2", "_1"]), ("aBc__long", ["-x", "-y", "-z"])):
            ids = [prefix + suffix for suffix in suffixes]
            for ident in ids:
                self.start(ident)
            selected = {"id_prefix": prefix}
            preview = self.resolve(selected)
            self.assertEqual((preview["matches"], preview["possible"], preview["actionable"]), (len(ids), 0, False))
            code, result = self.end(selected, ids[0])
            self.assertEqual((code, result["reason"], result["request_sent"]), (3, "ambiguous", False))
            self.assertTrue(all(self.alive(i) for i in ids))
            code, result = self.end({"id": ids[0]}, ids[0])
            self.assertEqual((code, result["result"], result["request_sent"]), (0, "accepted", True))
            self.assertTrue(all(self.alive(i) for i in ids[1:]))

    def test_conjunctive_command_detached_includes_own_attached_decoy(self):
        for ident in ("x.own", "x.target"):
            self.start(ident, ["/bin/sh", "-c", "sleep 59"])
        self.attach("x.own")
        self.env["KITTY_PTY_BROKER_SESSION"] = "x.own"
        selector = {"startup_words": ["sleep", "59"], "attached": False}
        preview = self.resolve(selector)
        self.assertTrue(preview["actionable"], "attachment must exclude the decoy before candidate counting")
        self.assertEqual((preview["matches"], preview["possible"], preview["resolved"]["id"]), (1, 0, "x.target"))
        code, result = self.end(selector, "x.target")
        self.assertEqual((code, result["result"]), (0, "accepted"))
        self.assertTrue(self.alive("x.own"))

    def test_startup_words_are_literal_contiguous_joined_argv(self):
        self.start("-Case", ["/bin/sh", "-c", "sleep 58", "arg with space", "é🚀"])
        for words, count in ((["sleep", "58"], 1), (["58", "arg", "with"], 1), (["é🚀"], 1),
                             (["Sleep", "58"], 0), (["sleep", "arg"], 0), (["sle"], 0), (['"sleep"'], 0)):
            with self.subTest(words=words):
                self.assertEqual(self.resolve({"startup_words": words})["matches"], count)
        self.assertEqual(self.resolve({"id": "-Case"})["resolved"]["id"], "-Case")

    def test_missing_truncated_malformed_unreadable_facts_cannot_exclude(self):
        self.start("goal", ["/bin/sh", "-c", "sleep 57"])
        self.start("decoy", ["/bin/sleep", "60"])
        metadata = self.runtime / "sessions/decoy/metadata"
        good = metadata.read_bytes()
        variants = [b"", good.replace(b"argv_truncated=0", b"argv_truncated=1"),
                    good.replace(b'argv_json=["/bin/sleep","60"]', b'argv_json=[]'),
                    good.replace(b"argv_json=", b"unknown="), good + b"argv_json=[]\n",
                    good + b"argv_truncated=0\n", good[:-1], good + b"\0",
                    good.replace(b"argv_json=", b"argv_json=bad"),
                    good.replace(b"started_millis=", b"ignored_started=")]
        for bad in variants:
            with self.subTest(bad=bad[-30:]):
                metadata.write_bytes(bad)
                preview = self.resolve({"startup_words": ["sleep", "57"]})
                self.assertEqual((preview["matches"], preview["possible"], preview["actionable"]), (1, 1, False))
                code, result = self.end({"startup_words": ["sleep", "57"]}, "goal")
                self.assertEqual((code, result["request_sent"]), (3, False))
                self.assertTrue(self.alive("goal"))
        metadata.unlink()
        os.mkfifo(metadata, 0o600)
        self.assertEqual(self.resolve({"startup_words": ["sleep", "57"]})["possible"], 1)
        metadata.unlink(); metadata.write_bytes(good); metadata.chmod(0o600)

    def test_socketless_and_unreachable_evidence_and_provisional_no_reaping(self):
        self.start("goal", ["/bin/sh", "-c", "sleep 56"])
        decoy = self.start("decoy", ["/bin/sleep", "60"])
        sock = self.runtime / "sessions/decoy/control.sock"
        saved = sock.with_name("held.sock")
        sock.rename(saved)
        self.assertEqual(self.resolve({"startup_words": ["sleep", "56"]})["resolved"]["id"], "goal")
        self.assertFalse(self.resolve({"id": "decoy"})["actionable"])
        self.assertTrue(sock.parent.exists())
        saved.rename(sock)
        os.kill(decoy["broker_pid"], signal.SIGSTOP)
        self.assertEqual(self.resolve({"startup_words": ["sleep", "56"]})["resolved"]["id"], "goal")
        self.assertEqual(self.resolve({"startup_words": ["/bin/sleep", "60"], "attached": False})["possible"], 1)
        os.kill(decoy["broker_pid"], signal.SIGCONT)
        provisional = self.runtime / "sessions/newborn"
        provisional.mkdir(mode=0o700)
        (provisional / ".spawn-metadata").write_text(f"broker_pid={os.getpid()}\n")
        original = sorted(provisional.iterdir())
        preview = self.resolve({"startup_words": ["sleep", "56"]})
        self.assertEqual((preview["matches"], preview["possible"]), (1, 1))
        self.assertEqual(original, sorted(provisional.iterdir()))
        code, result = self.end({"startup_words": ["sleep", "56"]}, "goal")
        self.assertEqual((code, result["request_sent"]), (3, False))

    def test_own_is_counted_then_guarded_and_missing_or_malformed_refuse(self):
        self.start("p.own"); self.start("p.other")
        self.env["KITTY_PTY_BROKER_SESSION"] = "p.own"
        self.assertEqual(self.resolve({"id_prefix": "p."})["matches"], 2)
        self.assertEqual(self.end({"id": "p.own"}, "p.own")[1]["reason"], "caller_guard")
        for caller in (None, "", "../bad"):
            env = dict(self.env)
            if caller is None:
                env.pop("KITTY_PTY_BROKER_SESSION", None)
            else:
                env["KITTY_PTY_BROKER_SESSION"] = caller
            code, result = self.end({"id": "p.other"}, "p.other", env=env)
            self.assertEqual((code, result["request_sent"]), (3, False))
            self.assertFalse(result["caller"]["authenticated"])
        self.assertTrue(self.alive("p.own")); self.assertTrue(self.alive("p.other"))

    def test_end_re_evaluates_after_preview_and_refuses_replacement_binding(self):
        old = self.start("bound")
        preview = self.resolve({"id": "bound"})
        self.call("kill", "bound", "--expect-started", str(old["started_millis"]))
        deadline = time.monotonic() + 3
        while (self.runtime / "sessions/bound").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        time.sleep(0.01)
        self.start("bound")
        code, result = self.end({"id": "bound"}, "bound", preview["resolved"]["started_millis"])
        self.assertEqual((code, result["reason"], result["request_sent"]), (3, "started_mismatch", False))
        self.assertTrue(self.alive("bound"))

    def test_receiver_attachment_guard_after_resolution(self):
        status = self.start("switch")
        self.assertFalse(self.resolve({"id": "switch", "attached": False})["resolved"]["attached"])
        os.kill(status["broker_pid"], signal.SIGSTOP)
        # Both connections are queued while the receiver is stopped. The attach
        # enters its accept queue first; attachment changes before termination.
        controller = self.connect("switch")
        frame(controller, 1, struct.pack(">HHHH", 24, 80, 0, 0))
        termination = self.connect("switch")
        frame(termination, 6, struct.pack(">QII", status["started_millis"], 0, 0))
        os.kill(status["broker_pid"], signal.SIGCONT)
        kind, payload = receive(termination)
        self.assertEqual((kind, payload), (10, b"attachment mismatch"))
        self.assertTrue(self.alive("switch"))

    def test_receiver_expected_start_guard(self):
        status = self.start("identity")
        termination = self.connect("identity")
        frame(termination, 6, struct.pack(">QII", status["started_millis"] + 1, 0, 0))
        self.assertEqual(receive(termination), (10, b"identity mismatch"))
        self.assertTrue(self.alive("identity"))

    def test_revalidates_startup_facts_after_census_before_dispatch(self):
        import threading
        status = self.start("probe", ["/bin/sh", "-c", "sleep 54"])
        path = self.runtime / "sessions/probe/control.sock"
        moved = path.with_name("original.sock")
        path.rename(moved)
        metadata = path.with_name("metadata")
        original = metadata.read_bytes()
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(str(path)); listener.listen(8); listener.settimeout(0.05)
        stopped = threading.Event()
        counts = {"status": 0, "terminate": 0}
        failures = []
        def serve():
            while not stopped.is_set():
                try:
                    peer, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                try:
                    with peer, socket.socket(socket.AF_UNIX) as target:
                        peer.settimeout(2); target.settimeout(2); target.connect(str(moved))
                        kind, payload = receive(peer)
                        if kind == 5:
                            counts["status"] += 1
                        if kind == 6:
                            counts["terminate"] += 1
                        frame(target, kind, payload)
                        response, body = receive(target)
                        if kind == 5 and counts["status"] == 2:
                            metadata.write_bytes(original.replace(b"sleep 54", b"sleep 53"))
                        frame(peer, response, body)
                except Exception as error:
                    failures.append(str(error))
        worker = threading.Thread(target=serve)
        worker.start()
        try:
            code, result = self.end({"startup_words": ["sleep", "54"]}, "probe")
            self.assertEqual((code, result["request_sent"]), (3, False))
            self.assertEqual(counts, {"status": 2, "terminate": 0})
            self.assertFalse(failures)
        finally:
            stopped.set(); worker.join(timeout=3); listener.close()
            restore_proxy_files(path, moved, metadata, original)
        self.assertTrue(self.alive("probe"))
        self.assertEqual(status["started_millis"], self.started["probe"]["started_millis"])

    def test_real_receiver_timeout_preserves_may_have_sent(self):
        status = self.start("timeout")
        helper = Path(CLI).with_name("selector-transport")
        if not helper.exists():
            self.skipTest("make test builds the test-only transport probe")
        os.kill(status["broker_pid"], signal.SIGSTOP)
        try:
            result = subprocess.run([str(helper), str(self.runtime), "timeout", str(status["started_millis"])],
                                    env=self.env, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads(result.stdout)
            self.assertEqual((doc["code"], doc["request_sent"], doc["receiver_refused"]), (10, True, False))
        finally:
            os.kill(status["broker_pid"], signal.SIGCONT)
        # The queued termination can still be accepted after the caller's timeout.
        deadline = time.monotonic() + 3
        while self.alive("timeout") and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(self.alive("timeout"))

    def test_client_distinguishes_sent_receiver_attachment_refusal(self):
        status = self.start("reject")
        helper = Path(CLI).with_name("selector-transport")
        if not helper.exists():
            self.skipTest("make test builds the test-only transport probe")
        self.attach("reject")
        result = subprocess.run([str(helper), str(self.runtime), "reject", str(status["started_millis"])],
                                env=self.env, capture_output=True, text=True, timeout=3)
        doc = json.loads(result.stdout)
        self.assertEqual((doc["code"], doc["request_sent"], doc["receiver_refused"]), (15, True, True))
        self.assertTrue(self.alive("reject"))

    def test_invalid_flags_and_values_refuse_with_bounded_output(self):
        self.start("guarded")
        for args in (("--id", "x" * 10000), ("--command-word", "x" * 60000),
                     ("--command-word", "x\x1by"), ("--id", "guarded", "--id-prefix", "g"),
                     ("--id", "guarded", "--caller", "override"), ("--id", "guarded", "--force")):
            with self.subTest(args=args[:1]):
                result = self.call("resolve", *args, "--json")
                self.assertEqual(result.returncode, 2)
                self.assertLess(len(result.stdout), 300)
        self.assertTrue(self.alive("guarded"))

    def test_bounded_complete_census_and_output(self):
        sessions = self.runtime / "sessions"
        sessions.mkdir(mode=0o700)
        for i in range(40):
            (sessions / f"d{i}").mkdir(mode=0o700)
        preview = self.resolve({"id_prefix": "d"})
        self.assertEqual((preview["matches"], len(preview["candidates"]), preview["candidates_truncated"]), (40, 32, True))
        for i in range(40, 1025):
            (sessions / f"d{i}").mkdir(mode=0o700)
        preview = self.resolve({"id_prefix": "d"})
        self.assertFalse(preview["complete"])
        self.assertFalse(preview["actionable"])
        self.assertEqual(preview["result"], "error")

    def test_older_receiver_refuses_required_attachment_without_fallback(self):
        if not OLD:
            self.skipTest("provide the committed old receiver binary")
        self.start("old", cli=OLD)
        code, result = self.end({"id": "old", "attached": False}, "old")
        self.assertEqual((code, result["reason"], result["request_sent"], result["receiver_refused"]),
                         (3, "cannot_bind", True, True))
        self.assertTrue(self.alive("old"))

    def test_fabricated_exact_selector_provenance_remains_unenforced(self):
        self.start("intent.first"); self.start("intent.second")
        self.assertEqual(self.resolve({"id_prefix": "intent."})["matches"], 2)
        # No API field carries authenticated original user intent. A caller can
        # fabricate this exact selector; the product must not claim to detect it.
        code, result = self.end({"id": "intent.second"}, "intent.second")
        self.assertEqual((code, result["result"]), (0, "accepted"))
        self.assertTrue(self.alive("intent.first"))


if __name__ == "__main__":
    unittest.main()
