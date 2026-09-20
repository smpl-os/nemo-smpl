#!/usr/bin/env python3
"""Exercise actual slot callbacks with cached-directory fixtures, without devices."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "src/nemo-window-slot.c").read_text()


def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


production = section("static void\nview_begin_loading_cb", "static void\nfilter_bar_cancel_cb")
production += section("static GtkWidget *\ncreate_mtp_unlock_box", "static void\nnemo_window_slot_init")
production += section("static void\nview_end_loading_cb", "static void\nnemo_window_slot_dispose")
flags = shlex.split(subprocess.check_output(
    ["pkg-config", "--cflags", "--libs", "gtk+-3.0"], text=True))
with tempfile.TemporaryDirectory(prefix="nemo-mtp-empty-") as tmp:
    directory = Path(tmp)
    (directory / "mtp-slot-production.inc").write_text(production)
    binary = directory / "test-mtp-empty"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-unused-function",
        "-I" + tmp, str(ROOT / "test/test-mtp-empty.c"), "-o", str(binary),
    ] + flags, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
