#!/usr/bin/env python3
"""Exercise the Amiga binaries through Vamos; no target-side fixture code."""

import argparse
import json
import pathlib
import re
import subprocess


def check(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vamos", required=True)
    parser.add_argument("--bin-dir", type=pathlib.Path, required=True)
    parser.add_argument("--cpu", choices=("68000", "68020"), default="68000")
    args = parser.parse_args()

    def run(name, *arguments):
        return subprocess.run(
            [args.vamos, "-C", args.cpu,
             str(args.bin_dir / name), *arguments],
            capture_output=True, text=True, timeout=15, check=False)

    names = ("ShowTasks", "WatchTasks")
    for name in names:
        result = run(name, "HELP")
        check(result.returncode, 0, f"{name} HELP rc")
        if not result.stdout.startswith(f"{name} "):
            raise AssertionError(f"{name} HELP has no version line: {result.stdout!r}")
        if f"$VER: {name} ".encode() not in (args.bin_dir / name).read_bytes():
            raise AssertionError(f"{name} has no $VER: tag")
        if "PURPOSE:" not in result.stdout or "USAGE:" not in result.stdout:
            raise AssertionError(f"{name} HELP incomplete: {result.stdout!r}")

    result = run("ShowTasks")
    check(result.returncode, 0, "ShowTasks table rc")
    lines = result.stdout.splitlines()
    if (len(lines) < 3 or not lines[1].startswith("TYPE TASK_ID") or
            not re.match(r"^P +[0-9A-F]{8} +[0-9]+ +[-0-9]+ +RUN +[0-9]+ +ShowTasks$",
                         lines[2]) or "SIGNALS" in result.stdout or "\x00" in result.stdout):
        raise AssertionError(f"ShowTasks table malformed: {result.stdout!r}")

    result = run("ShowTasks", "DETAIL")
    check(result.returncode, 0, "ShowTasks DETAIL rc")
    if "  SIGNALS   ALLOC " not in result.stdout or "  STACK     " not in result.stdout:
        raise AssertionError(f"ShowTasks DETAIL incomplete: {result.stdout!r}")

    result = run("ShowTasks", "MACHINE")
    check(result.returncode, 0, "ShowTasks rc")
    tasks = [json.loads(line) for line in result.stdout.splitlines()]
    if not any(task.get("name") == "ShowTasks" for task in tasks):
        raise AssertionError(f"ShowTasks output: {tasks!r}")
    for task in tasks:
        if not all(field in task for field in
                   ("task_id", "cli_id", "type", "priority", "state",
                    "signal_wait", "stack_lower", "stack_upper")):
            raise AssertionError(f"ShowTasks detail missing: {task!r}")

    result = run("ShowTasks", "TASK=FFFFFFFF", "MACHINE")
    check((result.returncode, result.stdout), (5, ""), "ShowTasks missing task JSON")
    result = run("ShowTasks", "CLI=9999")
    check((result.returncode, result.stdout),
          (5, "ShowTasks: no matching task\n"), "ShowTasks missing CLI")
    for selector in (("TASK=badhex",), ("CLI=0",),
                     ("TASK=FFFFFFFF", "CLI=1")):
        result = run("ShowTasks", *selector)
        check(result.returncode, 10, f"ShowTasks invalid {selector}")
    result = run("ShowTasks", "TASK=badhex", "MACHINE")
    check((result.returncode, result.stdout), (10, ""), "ShowTasks invalid JSON")

    result = run("WatchTasks", "MACHINE", "COUNT=1")
    check(result.returncode, 0, "WatchTasks rc")
    snapshots = [json.loads(line) for line in result.stdout.splitlines()]
    if snapshots[0].get("snapshot") != 1:
        raise AssertionError(f"WatchTasks output: {snapshots!r}")
    if not any("cpu_tenths" in row for row in snapshots if "task_id" in row):
        raise AssertionError(f"WatchTasks CPU output missing: {snapshots!r}")

    print("amiga-smoke: 2 commands passed")


if __name__ == "__main__":
    main()
