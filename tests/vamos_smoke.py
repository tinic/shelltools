#!/usr/bin/env python3
"""Exercise the Amiga binaries through Vamos; no target-side fixture code."""

import argparse
import json
import pathlib
import subprocess
import tempfile


def check(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vamos", required=True)
    parser.add_argument("--bin-dir", type=pathlib.Path, required=True)
    parser.add_argument("--cpu", choices=("68000", "68020"), default="68000")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="shelltools-vamos-") as tmp:
        fixture = pathlib.Path(tmp)
        (fixture / "lines.txt").write_bytes(b"alpha\nbeta\ngamma\ndelta\n")
        (fixture / "no-final.txt").write_bytes(b"first\nlast")

        def run(name, *arguments):
            return subprocess.run(
                [args.vamos, "-C", args.cpu, "-V", f"test:{fixture}",
                 str(args.bin_dir / name), *arguments],
                capture_output=True, text=True, timeout=15, check=False)

        names = ("ShowTasks", "WatchTasks", "BreakTask", "FirstLines",
                 "LastLines", "FindText", "SystemInfo")
        for name in names:
            result = run(name, "HELP")
            check(result.returncode, 0, f"{name} HELP rc")
            if "PURPOSE:" not in result.stdout or "USAGE:" not in result.stdout:
                raise AssertionError(f"{name} HELP incomplete: {result.stdout!r}")

        result = run("FirstLines", "FILE=test:lines.txt", "LINES=2")
        check((result.returncode, result.stdout), (0, "alpha\nbeta\n"), "FirstLines")
        result = run("FirstLines", "FILE=test:lines.txt", "LINES=0")
        check((result.returncode, result.stdout), (0, ""), "FirstLines zero")

        result = run("LastLines", "FILE=test:lines.txt", "LINES=2")
        check((result.returncode, result.stdout), (0, "gamma\ndelta\n"), "LastLines")
        result = run("LastLines", "FILE=test:no-final.txt", "LINES=1")
        check((result.returncode, result.stdout), (0, "last"), "LastLines no final LF")

        result = run("FindText", "TEXT=TA", "FILE=test:lines.txt", "NOCASE", "NUMBER")
        check((result.returncode, result.stdout), (0, "2:beta\n4:delta\n"), "FindText")
        result = run("FindText", "TEXT=missing", "FILE=test:lines.txt", "COUNT")
        check((result.returncode, result.stdout), (5, "0\n"), "FindText no match")

        result = run("SystemInfo", "MACHINE")
        check(result.returncode, 0, "SystemInfo rc")
        system = json.loads(result.stdout)
        if "exec_version" not in system or "chip_free" not in system:
            raise AssertionError(f"SystemInfo output: {system!r}")

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

        result = run("WatchTasks", "MACHINE", "COUNT=1")
        check(result.returncode, 0, "WatchTasks rc")
        snapshots = [json.loads(line) for line in result.stdout.splitlines()]
        if snapshots[0].get("snapshot") != 1:
            raise AssertionError(f"WatchTasks output: {snapshots!r}")
        if not any("cpu_tenths" in row for row in snapshots if "task_id" in row):
            raise AssertionError(f"WatchTasks CPU output missing: {snapshots!r}")

        result = run("BreakTask", "CLI=9999", "COMMAND=absent")
        check(result.returncode, 5, "BreakTask missing target rc")
        result = run("BreakTask", "TASK=FFFFFFFF", "NAME=absent")
        check(result.returncode, 5, "BreakTask missing task rc")

    print("amiga-smoke: 7 commands passed")


if __name__ == "__main__":
    main()
