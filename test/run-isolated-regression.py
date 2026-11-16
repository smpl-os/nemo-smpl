#!/usr/bin/env python3
"""Run a GTK regression without connecting to the user's desktop or settings."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def run_wayland(weston, command, env, root):
    env["WAYLAND_DISPLAY"] = "nemo-test-wayland"
    socket = Path(env["XDG_RUNTIME_DIR"]) / env["WAYLAND_DISPLAY"]
    with (root / "weston.log").open("w+") as log:
        compositor = subprocess.Popen(
            ["xvfb-run", "-a", weston, "--backend=x11", "--renderer=pixman", "--idle-time=0",
             "--no-config", f"--socket={env['WAYLAND_DISPLAY']}"],
            env=env, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 10
            while not socket.exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Private Wayland compositor failed to start")
                time.sleep(0.02)
            return subprocess.run(command, env=env, check=False).returncode
        finally:
            try:
                os.killpg(compositor.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            compositor.wait(timeout=10)
            log.seek(0)
            print(log.read(), file=sys.stderr)


def main():
    if len(sys.argv) < 2:
        print("usage: run-isolated-regression.py <test executable> [args...]", file=sys.stderr)
        return 2

    wayland = sys.argv[1] == "--wayland"
    arguments = sys.argv[2:] if wayland else sys.argv[1:]
    xvfb = shutil.which("weston" if wayland else "xvfb-run")
    dbus = shutil.which("dbus-run-session")
    if xvfb is None or dbus is None:
        print("SKIP: GTK regressions require a private display server and dbus-run-session.", file=sys.stderr)
        return 77

    # Runtime socket paths must stay short even inside a long build directory.
    with tempfile.TemporaryDirectory(prefix="nemo-regression-", dir="/tmp") as scratch:
        root = Path(scratch)
        env = os.environ.copy()
        for key in (
            "DISPLAY",
            "WAYLAND_DISPLAY",
            "XAUTHORITY",
            "DBUS_SESSION_BUS_ADDRESS",
            "DBUS_STARTER_ADDRESS",
            "DBUS_STARTER_BUS_TYPE",
            "AT_SPI_BUS_ADDRESS",
            "SESSION_MANAGER",
        ):
            env.pop(key, None)
        for key, name in (
            ("HOME", "home"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
            ("XDG_DATA_HOME", "data"),
            ("XDG_STATE_HOME", "state"),
            ("XDG_RUNTIME_DIR", "runtime"),
        ):
            directory = root / name
            directory.mkdir(mode=0o700)
            env[key] = str(directory)
        env.update(
            NEMO_TEST_PROFILE=str(root),
            GDK_BACKEND="wayland" if wayland else "x11",
            XDG_SESSION_TYPE="wayland" if wayland else "x11",
            GSETTINGS_BACKEND="memory",
            GIO_USE_VFS="local",
            GIO_USE_VOLUME_MONITOR="unix",
            NO_AT_BRIDGE="1",
            GTK_MODULES="",
            GTK_USE_PORTAL="0",
            G_DEBUG="fatal-criticals",
            NEMO_TEST_ISOLATED="1",
        )
        # Do not auto-start host desktop services (notably document-portal
        # FUSE mounts) inside a disposable test profile.
        bus_config = root / "session.conf"
        bus_config.write_text(
            "<busconfig><type>session</type>"
            f"<listen>unix:tmpdir={root / 'runtime'}</listen>"
            '<policy context="default"><allow own="*"/>'
            '<allow send_destination="*"/><allow receive_sender="*"/>'
            "</policy></busconfig>",
            encoding="utf-8",
        )
        command = [dbus, "--config-file", str(bus_config), "--", *arguments]
        if wayland:
            return run_wayland(xvfb, command, env, root)
        command = [xvfb, "-a", *command]
        return subprocess.run(command, env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
