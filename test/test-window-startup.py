#!/usr/bin/env python3
"""Construct real Nemo windows on the regression runner's private desktop."""

import os
from pathlib import Path
import subprocess
import sys
import time


def remain_running(process, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Nemo exited during window construction: {process.returncode}")
        time.sleep(0.05)


def main():
    if os.environ.get("NEMO_TEST_ISOLATED") != "1":
        print("Run through test/run-isolated-regression.py", file=sys.stderr)
        return 77
    profile = Path(os.environ["NEMO_TEST_PROFILE"])
    executable = str(Path(sys.argv[1]).resolve())
    other = profile / "second-window"
    other.mkdir()
    env = os.environ.copy()
    env.update(G_DEBUG="fatal-criticals", XDG_CURRENT_DESKTOP="X-Nemo-Test", GTK_USE_PORTAL="0")
    log_path = profile / "window-startup.log"
    with log_path.open("w+") as log:
        process = subprocess.Popen(
            [executable, "--no-desktop", env["HOME"]],
            env=env, stdout=log, stderr=log,
        )
        try:
            remain_running(process, 2)
            subprocess.run(
                [executable, "--no-desktop", str(other)],
                env=env, stdout=log, stderr=log, check=True, timeout=20,
            )
            remain_running(process, 1)
            subprocess.run(
                [executable, "--quit"], env=env, stdout=log, stderr=log,
                check=True, timeout=20,
            )
            if process.wait(timeout=10) != 0:
                raise RuntimeError(f"Nemo exited with status {process.returncode}")
        except (RuntimeError, subprocess.SubprocessError):
            log.flush()
            print(log_path.read_text(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print("Real Nemo window construction and shutdown passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
