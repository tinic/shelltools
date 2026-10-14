# shelltools

Two standalone AmigaOS 68k Shell commands for observing Exec tasks and AmigaDOS processes. Both use AmigaDOS-style names and ReadArgs templates, provide concise HELP, and expose MACHINE JSON output. No replacement scheduler or background service is required.

## Commands

| Command | Purpose | Key options |
| --- | --- | --- |
| ShowTasks | Exec task and DOS process details | TASK, CLI, DETAIL, MACHINE |
| WatchTasks | Live task CPU and memory monitor | TICKS, COUNT, ROWS, COLS, MACHINE |

Run Command HELP for exact syntax and return codes. No Unix aliases are installed.

Both commands handle Ctrl-C as an AmigaDOS break (return code 5).

ShowTasks reports the task address as task_id and pr_TaskNum as cli_id. It also emits process_number when pr_TaskNum is nonzero; otherwise process_number is null. This is a Shell process number, not a universal process ID. Task addresses can be reused after a task exits. WatchTasks verifies the selected task's address and name against the current Exec lists before sending Ctrl-C with `b`. Ctrl-C is advisory; the target decides whether to exit.

ShowTasks defaults to one aligned row per task with a P/T marker, task ID, CLI number, priority, state, stack size, and name. A different command appears on an indented line. `DETAIL` adds signal masks, stack addresses, process fields, and arguments; `MACHINE` emits one JSON object per task with all fields. Stack values are snapshots and a running task's saved stack pointer may be stale. Bounded name/command/argument copies avoid allocating or doing DOS I/O while Forbid() is active. A snapshot is capped at 96 tasks; truncation returns 5.

Use `ShowTasks TASK=<hex> DETAIL` or `ShowTasks CLI=<positive number> DETAIL` to inspect one captured task. `TASK` accepts the displayed hex ID with optional `$` or `0x` prefix. The selectors cannot be combined. A missing task returns 5; `MACHINE` emits no task record in that case. Invalid selectors return 10.

WatchTasks draws with Amiga console CSI sequences, one Write() a frame, and only the first four screen pens: blue title and key bars, a white column heading strip, a black selected row, and blue or black filled cells with ISO-8859-1 middle dots for the CPU and memory bars. Ordinary rows keep the Console Preferences default colors, and text and position carry every meaning that a color shows. The cursor is hidden while it runs and restored on exit. It reads the console's window bounds on startup and checks for resizing while running; ROWS and COLS override detection when needed. It updates only changed lines, including when selection moves. The title and selected task fill their full rows. The T/P column marks an Exec task (thread) with T and an AmigaDOS process with P. Task rows also show task ID, CLI number, priority, sampled CPU percentage, state, and the longest name the window permits; narrow windows retain T/P, CPU percentage, and name. The selected task's command appears in the detail line. In an interactive console: j/k or arrows select, n/u page, t/p/s/c/a sort by CPU/priority/state/CLI/name, / filters, Space pauses, b sends Ctrl-C to the selected task, h opens grouped scrollable help, and q quits. CPU usage is estimated from the task running at each vertical blank interrupt (50 or 60 samples per second); idle frames count toward the denominator. The server is removed when WatchTasks exits. Values below ten samples appear as unavailable. With redirected I/O it prints repeated detail snapshots; MACHINE emits JSON lines with `cpu_tenths` (percent times ten, or null when unavailable). Scout patches Exec task switching for more precise timing; this tool uses sampling to keep the monitor small and removable.

WatchTasks allocates a private 16 KB stack and switches back to the caller's stack before freeing it. This keeps its snapshots and console rendering within a known stack budget even when launched from a Shell with a small stack.

Press Return on a selected task to open a centered details overlay; the table around it is cleared until it closes. It shows the task name, type, address, CLI number, command and arguments, priority, state, sampled CPU usage, stack bounds, saved stack pointer, signal masks, and flags. Details stay attached to that task as the table reorders and update with each snapshot; if it disappears, the overlay marks the retained values as missing. Long text wraps and `j/k` or arrows scroll; `n/u` moves a page. Return or Esc closes the overlay and restores the table. Finishing a filter with Return keeps you in the table.

For disk capacity, use AmigaDOS `INFO` to list disks or `INFO DH0:` to select one. The A3000's installed `INFO ?` template accepts only `DEVICE`; it has no unit selector. Newer AmigaOS versions document [additional INFO options](https://wiki.amigaos.net/wiki/AmigaOS_Manual%3A_AmigaDOS_Command_Reference#INFO).

Use the existing AmigaDOS commands for other Shell operations: `STATUS` for Shell process status, `BREAK` for process signals, `SEARCH` for text search, `TYPE` for file contents, and `AVAIL` for free memory. Command options depend on the installed AmigaOS version; see the [AmigaDOS command reference](https://wiki.amigaos.net/wiki/AmigaOS_Manual%3A_AmigaDOS_Command_Reference).

## Build

Use [AmigaPorts GCC 16.2-rc16 or newer](https://github.com/AmigaPorts/m68k-amigaos-gcc/releases), CMake 3.25+, and Ninja. Point SHELLTOOLS_TOOLCHAIN_ROOT at the unpacked toolchain directory containing bin/m68k-amigaos-c++.

~~~sh
export SHELLTOOLS_TOOLCHAIN_ROOT=/path/to/m68k-amigaos-gcc-16.2
cmake --preset amiga-68000
cmake --build --preset amiga-68000
cmake --install build/amiga-68000 --prefix /path/to/staging
~~~

The optional 68020–68060 build is amiga-68020-60. Binaries install to C/ in the prefix. All tools use C++23, -Os -flto, section garbage collection, and disable RTTI and exceptions. The 68000 build uses the nix20 startup/runtime.

Both release builds use strict GCC warnings as errors, including the [C++ Best Practices warning set](https://github.com/cpp-best-practices/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#compilers) (`-Wall`, `-Wextra`, `-Wpedantic`, `-Wshadow`, conversion and cast checks, and related diagnostics). Run GCC's static analyzer and cppcheck with the analysis preset; cppcheck is required for this preset:

~~~sh
cmake --preset amiga-analyze
cmake --build --preset amiga-analyze
~~~

The analysis build disables LTO so GCC `-fanalyzer` runs. cppcheck checks all translation units. The analysis preset also exports `compile_commands.json`.

For target-level smoke checks with [Vamos](https://github.com/cnvogelg/amitools):

~~~sh
cmake --preset amiga-68000 -DSHELLTOOLS_VAMOS=/path/to/vamos
cmake --build --preset amiga-68000
ctest --preset amiga-68000
~~~

The tests exercise both CPU builds when configured with Vamos. They cover help, task tables and details, JSON output, and task/CLI selectors. Interactive console rendering and CPU sampling require an AmigaOS machine or emulator with vertical blank interrupts. The sibling AmiNetXDuo project has tests/tools/hwshell.py for physical Amiga shell interaction and tools/amiberry-run.sh plus tools/emu-rig-lock.sh for isolated Amiberry runs.

`tests/watchtasks_console.py` checks the details overlay through AmiNetXDuo's WebSocket shell: Return/Esc, wrapped text, scrolling, resizing, live identity, an exited task, and clean exit. It uses session 1 and requires Python's `pyte` package. Stage a uniquely named test copy in RAM: before running it:

~~~sh
curl --upload-file build/amiga-68000/C/WatchTasks 'http://amiga-3000.local/Ram%20Disk/WatchTasks-overlay-test'
python tests/watchtasks_console.py amiga-3000.local --hwshell /path/to/AmiNetXDuo/tests/tools/hwshell.py
~~~

For memory checks, AmiNetXDuo's `tools/enforcer-run.sh -m` boots an isolated 68030 FS-UAE guest with MungWall and Enforcer, then reports hits from the serial log. It needs separate copies of Enforcer, MungWall, and a Kickstart ROM through `AMINETXDUO_ENFORCER`, `AMINETXDUO_MUNGWALL`, and `AMINETXDUO_KICKSTART`. Run WatchTasks with `-a 'MACHINE COUNT=2 TICKS=10'` to exercise snapshots and exit. A `CON:` run also exercises the interactive exit path.

## Prior art

[Scout 3.6](https://www.aminet.net/util/moni/Scout_os3.readme) is a broader GUI system monitor and a useful reference for task details and signals. Older Shell tools include [PS](https://wiki.aminet.net/Contents_of_Aminet_CD_4) for task and process listings, and [Executive's Top and Ps](https://aminet.net/package/util/misc/Executive), which depend on its background server. This project provides standalone task inspection and an interactive Shell monitor with readable text and structured output. On GitHub, [amimcp](https://github.com/thomas-luebker/amimcp) and [AmigaAI](https://github.com/murinsel/AmigaAI) provide model-to-machine bridges. Both commands can run inside such a bridge and return compact, structured observations.
