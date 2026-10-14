# shelltools

Small AmigaOS 68k Shell commands for observing and controlling a machine. Commands use AmigaDOS-style names and ReadArgs templates. Every command has HELP; observation commands expose MACHINE JSON output.

## Commands

| Command | Purpose | Key options |
| --- | --- | --- |
| ShowTasks | Exec task and DOS process details | MACHINE |
| WatchTasks | Live task CPU and memory monitor | TICKS, COUNT, ROWS, COLS, MACHINE |
| BreakTask | Send Ctrl-C to a verified task | CLI + COMMAND, or TASK + NAME |
| SystemInfo | OS and hardware information | MACHINE |
| FirstLines | First lines of a file or input stream | FILE, LINES |
| LastLines | Last lines of a seekable file | FILE, LINES |
| FindText | Literal line search | TEXT, FILE, NOCASE, NUMBER, INVERT, COUNT |
| DiskFree | Filesystem capacity through Info() | PATH, MACHINE |
| RebootSync | Invoke AmigaOS 3.2 C:Reboot SYNC | HELP |

Run Command HELP for exact syntax and return codes. No Unix aliases are installed.

ShowTasks reports the task address as task_id and pr_TaskNum as cli_id. It also emits process_number when pr_TaskNum is nonzero; otherwise process_number is null. This is a Shell process number, not a universal process ID. Task addresses can be reused after a task exits. BreakTask TASK=<hex> NAME=<exact name> verifies both values against a current Exec list while task switching is forbidden, then sends Ctrl-C. The CLI route similarly checks the current command name. Ctrl-C is advisory; the target decides whether to exit.

ShowTasks detail rows and JSON include type, state, priority, task flags, signal masks, stack bounds, saved stack pointer, process stack size/flags, task name, command, and arguments. Stack values are snapshots and a running task's saved stack pointer may be stale. Bounded name/command/argument copies avoid allocating or doing DOS I/O while Forbid() is active. A snapshot is capped at 96 tasks; truncation returns 5.

WatchTasks uses ISO-8859-1 guillemets and middle dots for memory bars, and Amiga console CSI sequences for repainting. It reads the console's window bounds on startup and checks for resizing while running; ROWS and COLS override detection when needed. It updates only changed lines, including when selection moves. The title and selected task fill their full rows. Task rows show task ID, CLI number, priority, sampled CPU percentage, state, and the longest name the window permits; narrow windows retain CPU percentage and name. The selected task's command appears in the detail line. In an interactive console: j/k or arrows select, n/u page, t/p/s/c/a sort by CPU/priority/state/CLI/name, / filters, Space pauses, b sends Ctrl-C to the selected task, h shows keys, and q quits. CPU usage is estimated from the task running at each vertical blank interrupt (50 or 60 samples per second); idle frames count toward the denominator. The server is removed when WatchTasks exits. Values below ten samples appear as unavailable. With redirected I/O it prints repeated detail snapshots; MACHINE emits JSON lines with `cpu_tenths` (percent times ten, or null when unavailable). Scout patches Exec task switching for more precise timing; this tool uses sampling to keep the monitor small and removable.

SystemInfo opens identify.library at runtime and reports system, CPU, FPU, MMU, OS, chipset, clock, graphics, and audio when installed. It falls back to Exec version and Chip/Fast free memory without the library. Install a current identify.library in LIBS: for the full report. RebootSync uses the OS 3.2 Reboot SYNC command, which attempts to inhibit partitions before reboot. It rejects older Exec versions.

## Build

Use [AmigaPorts GCC 16.2-rc16 or newer](https://github.com/AmigaPorts/m68k-amigaos-gcc/releases), CMake 3.25+, and Ninja. Point SHELLTOOLS_TOOLCHAIN_ROOT at the unpacked toolchain directory containing bin/m68k-amigaos-c++.

~~~sh
export SHELLTOOLS_TOOLCHAIN_ROOT=/path/to/m68k-amigaos-gcc-16.2
cmake --preset amiga-68000
cmake --build --preset amiga-68000
cmake --install build/amiga-68000 --prefix /path/to/staging
~~~

The optional 68020–68060 build is amiga-68020-60. Binaries install to C/ in the prefix. All tools use C++23, -Os -flto, section garbage collection, and disable RTTI and exceptions. The 68000 build uses the nix20 startup/runtime.

For target-level smoke checks with [Vamos](https://github.com/cnvogelg/amitools):

~~~sh
cmake --preset amiga-68000 -DSHELLTOOLS_VAMOS=/path/to/vamos
cmake --build --preset amiga-68000
ctest --preset amiga-68000
~~~

The tests exercise both CPU builds when configured with Vamos. They cover help, line handling, search, JSON output, and stale task targeting. Interactive console rendering and CPU sampling require an AmigaOS machine or emulator with vertical blank interrupts. The sibling AmiNetXDuo project has tests/tools/hwshell.py for physical Amiga shell interaction and tools/amiberry-run.sh plus tools/emu-rig-lock.sh for isolated Amiberry runs.

## Prior art

[Scout 3.6](https://www.aminet.net/util/moni/Scout_os3.readme) already covers a much broader system monitor, including task details, signals, and hardware inventory. It is a useful reference for field selection; this project focuses on small Shell commands and structured output. [identify.library](https://github.com/shred/identify) provides the optional hardware detection. [AmigaOS 3.2 Reboot SYNC](https://wiki.amigaos.net/wiki/AmigaOS_Manual:_AmigaDOS_Command_Reference) supplies the filesystem-aware reboot operation. AmiNet also has older top, ps, headtail, and grep packages; AmigaOS itself includes STATUS, BREAK, SEARCH, INFO, and AVAIL. On GitHub, [amimcp](https://github.com/thomas-luebker/amimcp) and [AmigaAI](https://github.com/murinsel/AmigaAI) provide model-to-machine bridges. These commands can run inside such a bridge and return compact, structured observations.
