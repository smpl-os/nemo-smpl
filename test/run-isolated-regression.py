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
    arguments = sys.argv[1:]
    wayland = False
    gvfs = False
    while arguments and arguments[0] in ("--wayland", "--gvfs"):
        option = arguments.pop(0)
        wayland |= option == "--wayland"
        gvfs |= option == "--gvfs"
    if not arguments:
        print("usage: run-isolated-regression.py [--wayland] [--gvfs] <test executable> [args...]",
              file=sys.stderr)
        return 2

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
            GIO_USE_VFS="gvfs" if gvfs else "local",
            GIO_USE_VOLUME_MONITOR="unix",
            NO_AT_BRIDGE="1",
            GTK_MODULES="",
            GTK_USE_PORTAL="0",
            G_DEBUG="fatal-criticals",
            NEMO_TEST_ISOLATED="1",
        )
        # Only explicitly requested GVfs services may start on the private bus.
        services_xml = ""
        if gvfs:
            services = root / "services"
            services.mkdir(mode=0o700)
            data_dirs = os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":")
            for name in ("Daemon", "Metadata"):
                filename = f"org.gtk.vfs.{name}.service"
                source = next((Path(directory) / "dbus-1/services" / filename
                               for directory in data_dirs
                               if (Path(directory) / "dbus-1/services" / filename).is_file()), None)
                if source is None:
                    print(f"SKIP: GVfs regressions require {filename}.", file=sys.stderr)
                    return 77
                contents = source.read_text(encoding="utf-8")
                if name == "Daemon":
                    contents = "\n".join(line + " --no-fuse" if line.startswith("Exec=") else line
                                         for line in contents.splitlines()) + "\n"
                (services / filename).write_text(contents, encoding="utf-8")
            services_xml = f"<servicedir>{services}</servicedir>"
        bus_config = root / "session.conf"
        bus_config.write_text(
            "<busconfig><type>session</type>"
            f"<listen>unix:tmpdir={root / 'runtime'}</listen>"
            f"{services_xml}"
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
