"""Run the real CLI from a relocated staged installation."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="broker-install-") as temporary:
    stage = Path(temporary) / "stage"
    subprocess.run(["make", "-C", str(root), f"BUILD_DIR={build}",
                    f"DESTDIR={stage}", "PREFIX=/usr", "install"], check=True)
    relocated = Path(temporary) / "relocated prefix"
    (stage / "usr").rename(relocated)
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD"):
        env.pop(key, None)
    executable = relocated / "bin/kitty-pty-broker"
    result = subprocess.run([str(executable), "--help"], env=env,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert "kitty-pty-broker" in result.stdout + result.stderr
    # Verify the loader actually selected the staged library, even on machines
    # where a system copy is available and could hide a broken search path.
    env["LD_TRACE_LOADED_OBJECTS"] = "1"
    loaded = subprocess.run([str(executable)], env=env, capture_output=True,
                            text=True, check=True, timeout=10)
    assert str(relocated / "bin/../lib/libkitty-pty-broker.so") in loaded.stdout, loaded.stdout
print("relocated installed CLI loads its staged library")
