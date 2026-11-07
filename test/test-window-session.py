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
    unplugged = profile / "unplugged-drive"
    first.mkdir()
    second.mkdir()
    unplugged.mkdir()
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
                ([str(unplugged)], unplugged, None),
                ([], None, None),
                ([], None, None),
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
                if expected == unplugged:
                    unplugged.rmdir()
            # Overview bypasses directory loading; it must still release the
            # startup visibility gate and cancel a split pane's pending load.
            for left, right, active in (
                (["overview:///"], [], 0),
                ([first.as_uri(), "overview:///", second.as_uri()], [], 1),
                (["overview:///", first.as_uri()], ["overview:///"], 0),
            ):
                for key, value in (
                    ("saved-tabs-left", repr(left)),
                    ("saved-tabs-right", repr(right)),
                    ("saved-split-view", str(bool(right)).lower()),
                    ("saved-active-tab-left", str(active)),
                    ("saved-active-tab-right", "0"),
                    ("saved-active-pane", "1" if right else "0"),
                ):
                    subprocess.run(
                        ["gsettings", "set", "org.nemo.window-state", key, value],
                        env=env, check=True, timeout=10,
                    )
                process = subprocess.Popen(
                    [executable, "--no-desktop"], env=env, stdout=log, stderr=log,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", "overview - overview:///"],
                    env=env, check=True, timeout=15,
                )
                # A second no-argument invocation must reuse the primary's
                # D-Bus service and create another mapped restored window.
                subprocess.run(
                    [executable, "--no-desktop"], env=env, stdout=log, stderr=log,
                    check=True, timeout=15,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", "overview - overview:///", "2"],
                    env=env, check=True, timeout=15,
                )
                if process.poll() is not None:
                    raise RuntimeError("Primary Nemo exited during D-Bus reuse")
                subprocess.run(
                    [executable, "--no-desktop", str(first)],
                    env=env, stdout=log, stderr=log, check=True, timeout=15,
                )
                subprocess.run(
                    [inspector, "--wait-for-title", f"{first.name} - {first}"],
                    env=env, check=True, timeout=15,
                )
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
    print("Folder/Overview restoration, mapped windows and no-arg D-Bus reuse passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
