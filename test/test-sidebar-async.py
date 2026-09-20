#!/usr/bin/env python3
"""Compile the actual private async helpers against deterministic GIO fixtures.

No display, session bus, mounts, or user data are accessed. Only the surrounding
Nemo/GTK objects and I/O dispatch are replaced; cache/lifetime/scheduling code is
extracted verbatim from the production sources.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def main():
    sidebar = (ROOT / "src/nemo-places-sidebar.c").read_text()
    file = (ROOT / "libnemo-private/nemo-file.c").read_text()
    code = section(sidebar, "#define DISK_FULL_QUERY_TIMEOUT_SECONDS",
                   "#endif /* NEMO_SMPL */")
    code += section(sidebar, "static gint\nget_disk_full",
                    "static gboolean\nhome_on_different_fs")
    code += section(sidebar, "static gboolean\nhome_on_different_fs",
                    "static gchar *\nget_icon_name")
    code += section(sidebar, "static GList *\nget_portable_devices_from_media_dir",
                    "/* Detect if an MTP-capable USB interface")
    code += section(sidebar, "static gboolean\nupdate_places_on_idle_callback",
                    "static void\nnemo_places_sidebar_set_parent_window")
    code += section(file, "static GFile *\nresolve_computer_target",
                    "typedef struct _NemoFilesystemQuery")
    code += section(file, "typedef struct _NemoFilesystemQuery",
                    "#else\nstatic GFile *\nget_filesystem_query_location_for_computer")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-unix-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="nemo-sidebar-async-") as tmp:
        path = Path(tmp)
        (path / "sidebar-production.inc").write_text(code)
        binary = path / "test-sidebar-async"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-function",
            "-Wno-deprecated-declarations",
            "-I" + tmp, str(ROOT / "test/test-sidebar-async.c"),
            "-o", str(binary)] + flags + ["-lm"], check=True)
        subprocess.run([str(binary)], check=True,
                       env={**os.environ, "GIO_USE_VFS": "local", "GIO_USE_VOLUME_MONITOR": "unix"},
                       timeout=30)


if __name__ == "__main__":
    main()
