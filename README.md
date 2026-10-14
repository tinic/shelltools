# shelltools

Small AmigaOS 68k Shell commands for observing and controlling a machine. Commands use AmigaDOS-style names and ReadArgs templates. Every command has HELP; observation commands expose MACHINE JSON output.

## Commands

| Command | Purpose | Key options |
| --- | --- | --- |
| ShowTasks | Exec task and DOS process details | MACHINE |
| WatchTasks | Live console task monitor | TICKS, COUNT, ROWS, MACHINE |
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

WatchTasks uses ISO-8859-1 guillemets and middle dots for memory bars, and Amiga console CSI sequences for repainting. In an interactive console: j/k or arrows select, n/u page, p/s/c/a sort by priority/state/CLI/name, / filters, Space pauses, b sends Ctrl-C to the selected task, h shows keys, and q quits. Set ROWS to the console's text height (default 24). With redirected I/O it prints repeated detail snapshots; MACHINE emits JSON lines. Classic Exec does not expose a portable per-task CPU time counter. Scout obtains CPU usage by patching system functions, so this tool labels CPU percentages unavailable.

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

The tests exercise both CPU builds when configured with Vamos. They cover help, line handling, search, JSON output, and stale task targeting. They do not exercise an interactive CON: window, native identify.library, or an actual reboot. Those require an AmigaOS emulator or machine. The sibling AmiNetXDuo project has tests/tools/hwshell.py for A1200 shell interaction and tools/amiberry-run.sh plus tools/emu-rig-lock.sh for isolated Amiberry runs. Validate the console UI and optional library on those rigs before shipping.

## Prior art

[Scout 3.6](https://www.aminet.net/util/moni/Scout_os3.readme) already covers a much broader system monitor, including task details, signals, and hardware inventory. It is a useful reference for field selection; this project focuses on small Shell commands and structured output. [identify.library](https://github.com/shred/identify) provides the optional hardware detection. [AmigaOS 3.2 Reboot SYNC](https://wiki.amigaos.net/wiki/AmigaOS_Manual:_AmigaDOS_Command_Reference) supplies the filesystem-aware reboot operation. AmiNet also has older top, ps, headtail, and grep packages; AmigaOS itself includes STATUS, BREAK, SEARCH, INFO, and AVAIL. On GitHub, [amimcp](https://github.com/thomas-luebker/amimcp) and [AmigaAI](https://github.com/murinsel/AmigaAI) provide model-to-machine bridges. These commands can run inside such a bridge and return compact, structured observations.
