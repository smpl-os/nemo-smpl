#!/usr/bin/env python3
"""Run a GTK regression without connecting to the user's desktop or settings."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        print("usage: run-isolated-regression.py <test executable> [args...]", file=sys.stderr)
        return 2

    xvfb = shutil.which("xvfb-run")
    dbus = shutil.which("dbus-run-session")
    if xvfb is None or dbus is None:
        print("SKIP: GTK regressions require xvfb-run and dbus-run-session.", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory(prefix="nemo-regression-") as scratch:
        root = Path(scratch)
        env = os.environ.copy()
        for key in (
            "DISPLAY",
            "WAYLAND_DISPLAY",
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
            GDK_BACKEND="x11",
            XDG_SESSION_TYPE="x11",
            GSETTINGS_BACKEND="memory",
            GIO_USE_VFS="local",
            GIO_USE_VOLUME_MONITOR="unix",
            NO_AT_BRIDGE="1",
            GTK_MODULES="",
            G_DEBUG="fatal-criticals",
            NEMO_TEST_ISOLATED="1",
        )
        # Start the bus inside Xvfb so activated services inherit its display.
        command = [xvfb, "-a", dbus, "--", *sys.argv[1:]]
        return subprocess.run(command, env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
