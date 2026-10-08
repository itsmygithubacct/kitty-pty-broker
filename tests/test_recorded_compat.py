"""Metadata/display compatibility on either side of the recorded-command change."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PRE_RECORDED = "9a0a93da2d3c7c68d359cb465008c19f736229bc"

with tempfile.TemporaryDirectory(prefix="kbrec-compat.") as scratch:
    scratch = Path(scratch)
    old = scratch / "old"
    old.mkdir()
    env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": str(scratch), "LANG": "C.UTF-8"}

    def run(*command, check=True):
        return subprocess.run(command, env=env, check=check, capture_output=True, text=True,
                              stdin=subprocess.DEVNULL, timeout=60)

    run("git", "-C", str(ROOT), "archive", PRE_RECORDED, "--output=" + str(scratch / "old.tar"))
    run("tar", "-xf", str(scratch / "old.tar"), "-C", str(old))
    run("make", "-C", str(old), "--silent")
    run("make", "-C", str(ROOT), "BUILD_DIR=" + str(scratch / "new-build"), "--silent")
    binaries = {"old": str(old / "build/kitty-pty-broker"), "new": str(scratch / "new-build/kitty-pty-broker")}
    pids = []
    try:
        for producer, consumer in (("old", "new"), ("new", "old")):
            rt = scratch / (producer + "-rt")
            rt.mkdir(mode=0o700)
            source = [binaries[producer], "--runtime-dir", str(rt)]
            client = [binaries[consumer], "--runtime-dir", str(rt)]
            run(*source, "run", "--id", "target", "--", "/bin/sleep", "90")
            live = json.loads(run(*client, "status", "target", "--json").stdout)
            pids.extend((live["broker_pid"], live["child_pid"]))
            rows = json.loads(run(*client, "list", "--json", "--all").stdout)
            assert len(rows) == 1 and rows[0]["reachable"] and "recorded" not in rows[0]
            os.kill(live["broker_pid"], signal.SIGSTOP)
            row = json.loads(run(*client, "--timeout", "0.1", "list", "--json", "--all").stdout)[0]
            assert not row["reachable"] and row["error"] == "timeout"
            if consumer == "new":
                assert row["recorded"] == {"argv": None, "cwd": None,
                                           "started_millis": live["started_millis"], "truncated": False}
            else:
                assert "recorded" not in row
                new_row = json.loads(run(*source, "--timeout", "0.1", "list", "--json", "--all").stdout)[0]
                assert new_row["recorded"]["argv"] == ["/bin/sleep", "90"]
            os.kill(live["broker_pid"], signal.SIGCONT)
            run(*client, "kill", "target", "--expect-started", str(live["started_millis"]))
            print(f"pass  {consumer} client / {producer} broker: live fields, unreachable display, bound kill")

        # The old stale-proof parser must ignore the extra JSON lines too.
        rt = scratch / "reap-rt"
        rt.mkdir(mode=0o700)
        source = [binaries["new"], "--runtime-dir", str(rt)]
        run(*source, "run", "--id", "corpse", "--", "/bin/sleep", "90")
        live = json.loads(run(*source, "status", "corpse", "--json").stdout)
        pids.extend((live["broker_pid"], live["child_pid"]))
        os.kill(live["broker_pid"], signal.SIGKILL)
        os.kill(live["child_pid"], signal.SIGKILL)
        # kill(pid, 0) still succeeds for a zombie. Let the subreaper collect
        # the owned broker before asking the old reader for its stale proof.
        deadline = time.monotonic() + 3
        while Path(f"/proc/{live['broker_pid']}").exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        assert not Path(f"/proc/{live['broker_pid']}").exists(), "owned broker not reaped"
        # A descriptor can briefly outlive its broker, so a genuine system row
        # allows another listing; a silent listing must already have reaped it.
        for _ in range(20):
            rows = json.loads(run(binaries["old"], "--runtime-dir", str(rt), "list", "--json", "--all").stdout)
            if not rows:
                assert not (rt / "sessions/corpse").exists()
                break
            time.sleep(0.02)
        else:
            raise AssertionError("old reader did not reap new metadata")
        print("pass  old metadata parser ignores new lines and reaps a proved corpse")
    finally:
        for pid in pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
