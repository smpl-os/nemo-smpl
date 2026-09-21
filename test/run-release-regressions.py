#!/usr/bin/env python3
"""Fail closed on missing, skipped, or failing copy/move release regressions."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid


REQUIRED_SUITES = {"copy-integrity", "copy-selection"}
SUITES = REQUIRED_SUITES | {"transfer-safety"}
CROSSFS_TEST = "copy-integrity-real-crossfs-move"
REQUIRED_TESTS = {
    CROSSFS_TEST,
    "Copy source selection",
    "test-progress-completion",
    "Completion real backends",
    "Completion pending backend exit",
}
TOOLS = ("meson", "dbus-run-session", "dbus-daemon", "xvfb-run", "Xvfb", "xauth")
SKIPPED_TAP = re.compile(
    r"^\s*(?:(?:ok\b|not ok\b|1\.\.\d+\b).*#\s*(?:SKIP|TODO)\b|1\.\.0(?:\s|$))",
    re.IGNORECASE | re.MULTILINE,
)


class GateError(Exception):
    pass


def select_tests(tests):
    selected = []
    found_suites = set()
    for test in tests:
        suites = {suite.rsplit(":", 1)[-1] for suite in test["suite"]} & SUITES
        found_suites.update(suites)
        if suites or test["name"] in REQUIRED_TESTS:
            selected.append(test)
    names = [test["name"] for test in selected]
    missing_suites = REQUIRED_SUITES - found_suites
    missing_tests = REQUIRED_TESTS - set(names)
    if missing_suites or missing_tests:
        raise GateError(
            f"Required safety coverage is absent: suites={sorted(missing_suites)}, "
            f"tests={sorted(missing_tests)}. Enable smpl_features and configure "
            "with the D-Bus/Xvfb test dependencies installed."
        )
    if len(set(names)) != len(names):
        raise GateError("Safety test names must be unique for exact result accounting.")
    return selected


def check_results(log_path, expected):
    with log_path.open(encoding="utf-8") as log:
        results = [json.loads(line) for line in log if line.strip()]
    # Meson versions decorate names with project/suite prefixes differently.
    names = [
        result["name"].rsplit(" / ", 1)[-1].rsplit(":", 1)[-1]
        for result in results
    ]
    if Counter(names) != Counter(expected):
        missing = list((Counter(expected) - Counter(names)).elements())
        extra = list((Counter(names) - Counter(expected)).elements())
        raise GateError(f"Incomplete safety results: missing={missing}, unexpected={extra}")
    for name, result in zip(names, results):
        if result["result"] != "OK" or result["returncode"] != 0:
            raise GateError(f"{name}: expected OK without skips, got {result['result']}")
        # Exit-code tests can pass even if GLib skips individual TAP subtests.
        if SKIPPED_TAP.search(result.get("stdout", "")):
            raise GateError(f"{name}: skipped, TODO, or empty TAP subtests are not release coverage")


def run(build_dir):
    missing = [tool for tool in TOOLS if shutil.which(tool) is None]
    if missing:
        raise GateError(f"Missing release safety test dependencies: {', '.join(missing)}")

    # Regenerate first so discovery cannot use stale test registrations.
    subprocess.run(["meson", "setup", "--reconfigure", str(build_dir)], check=True)
    metadata = subprocess.run(
        ["meson", "introspect", "--tests", str(build_dir)],
        check=True, stdout=subprocess.PIPE, text=True,
    )
    selected = select_tests(json.loads(metadata.stdout))
    names = [test["name"] for test in selected]
    crossfs = next(test for test in selected if test["name"] == CROSSFS_TEST)
    fixture_dir = Path(crossfs["workdir"] or build_dir)
    if "NEMO_TEST_CROSS_FS_ROOT" in crossfs["env"]:
        raise GateError("The cross-filesystem test must inherit the gate's disposable root.")

    # An override is a parent directory, never a fixture itself. Only our unique
    # child is removed; no mounts or existing files on that filesystem are changed.
    # The scenario chooses which side uses this secondary filesystem; durable
    # destinations belong in the build workspace, not the default tmpfs root.
    crossfs_parent = Path(os.environ.get("NEMO_TEST_CROSS_FS_ROOT", "/dev/shm")).resolve()
    with tempfile.TemporaryDirectory(
        prefix="nemo-release-crossfs-", dir=crossfs_parent,
    ) as crossfs_root:
        fixture_device = fixture_dir.stat().st_dev
        crossfs_device = Path(crossfs_root).stat().st_dev
        if fixture_device == crossfs_device:
            raise GateError(
                f"{fixture_dir} and {crossfs_parent} are on the same filesystem "
                f"(st_dev={fixture_device}). Set NEMO_TEST_CROSS_FS_ROOT to a writable "
                "directory on a different filesystem; release coverage cannot skip this test."
            )
        print(
            f"Release safety gate: {len(names)} tests; workspace device "
            f"{fixture_device}, secondary device {crossfs_device}", flush=True,
        )
        env = os.environ.copy()
        env["NEMO_TEST_CROSS_FS_ROOT"] = crossfs_root
        # Unique logs prevent an old passing run from satisfying this invocation.
        logbase = "release-safety-" + uuid.uuid4().hex
        result = subprocess.run(
            ["meson", "test", "-C", str(build_dir), "--print-errorlogs",
             "--logbase", logbase, *names],
            env=env, check=False,
        )
        if result.returncode != 0:
            raise GateError(f"Meson safety regressions failed (exit {result.returncode}).")
        log_path = build_dir / "meson-logs" / (logbase + ".json")
        check_results(log_path, names)
        print(f"Release safety gate passed: {len(names)} tests, no skips. Results: {log_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="Configured Meson build directory")
    args = parser.parse_args()
    try:
        run(args.build_dir.resolve())
    except (GateError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"Release safety gate FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
