#!/usr/bin/env python3
"""Verify saved folders survive a real process exit in a private profile."""

import os
from pathlib import Path
import subprocess
import sys


def main():
    if os.environ.get("NEMO_TEST_ISOLATED") != "1":
        return 77
    profile = Path(os.environ["NEMO_TEST_PROFILE"])
    executable = str(Path(sys.argv[1]).resolve())
    inspector = str(Path(sys.argv[2]).resolve())
    first = profile / "remember-first"
    second = profile / "remember-second"
    first.mkdir()
    second.mkdir()
    env = os.environ.copy()
    env.update(
        GSETTINGS_BACKEND="keyfile", G_DEBUG="fatal-criticals",
        XDG_CURRENT_DESKTOP="X-Nemo-Test", GTK_USE_PORTAL="0",
        LC_ALL="C", LANGUAGE="C",
    )
    subprocess.run(
        ["gsettings", "set", "org.nemo.preferences", "show-full-path-titles", "true"],
        env=env, check=True, timeout=10,
    )
    log_path = profile / "window-session.log"
    process = None
    with log_path.open("w+") as log:
        try:
            cases = [
                ([str(first)], first, None),
                ([], first, None),
                ([str(second)], second, None),
                ([], None, "false"),
                ([], second, "true"),
            ]
            for args, expected, restore in cases:
                if restore is not None:
                    subprocess.run(
                        ["gsettings", "set", "org.nemo.preferences",
                         "restore-tabs-on-startup", restore],
                        env=env, check=True, timeout=10,
                    )
                process = subprocess.Popen(
                    [executable, "--no-desktop", *args],
                    env=env, stdout=log, stderr=log,
                )
                title = f"{expected.name} - {expected}" if expected is not None else "Home"
                subprocess.run(
                    [inspector, "--wait-for-title", title], env=env,
                    check=True, timeout=15,
                )
                if process.poll() is not None:
                    raise RuntimeError(f"Nemo exited while opening {title}: {process.returncode}")
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
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print("Folder restoration across process exits passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
