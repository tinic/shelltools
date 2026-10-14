#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <exec/nodes.h>
#include <proto/dos.h>

#include <algorithm>
#include <span>

int main() {
    LONG args[5] = {};
    RDArgs *rd = ReadArgs("MACHINE/S,DETAIL/S,TASK/K,CLI/N/K,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("ShowTasks");
    }
    if (args[4]) {
        shelltools::help("PURPOSE: List Exec tasks and AmigaDOS processes\n"
                         "USAGE: ShowTasks [TASK=<hex> | CLI=<number>] [DETAIL] [MACHINE]\n"
                         "DEFAULT: Aligned task table; P=process T=task\n"
                         "TASK/CLI: Select one task; CLI must be positive\n"
                         "DETAIL: Signals, stack addresses, process fields\n"
                         "MACHINE: JSON lines; no match gives no output\n"
                         "RC: 0=ok 5=missing/truncated 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const bool machine = args[0] != 0;
    const bool detail = args[1] != 0;
    const bool by_task = args[2] != 0;
    const bool by_cli = args[3] != 0;
    ULONG task_id = 0;
    LONG cli_id = 0;
    if (by_cli) {
        cli_id = *reinterpret_cast<const LONG *>(args[3]);
    }
    if ((by_task && by_cli) ||
        (by_task && !shelltools::parse_task_id(reinterpret_cast<const char *>(args[2]), task_id)) ||
        (by_cli && cli_id <= 0)) {
        if (!machine) {
            PutStr("ShowTasks: use TASK=<hex> or CLI=<positive number>; see HELP\n");
        }
        FreeArgs(rd);
        return shelltools::error;
    }
    FreeArgs(rd);

    static shelltools::TaskSnapshot snapshot;
    shelltools::capture_tasks(snapshot);
    const auto rows = std::span{snapshot.rows}.first(snapshot.count);
    if (by_task || by_cli) {
        const auto found =
            std::find_if(rows.begin(), rows.end(), [&](const shelltools::TaskRow &row) {
                return by_task ? row.address == task_id
                               : row.type == NT_PROCESS && row.cli == cli_id;
            });
        if (found == rows.end()) {
            if (!machine) {
                PutStr("ShowTasks: no matching task\n");
            }
            if (snapshot.truncated) {
                PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
            }
            return shelltools::warn;
        }
        if (!machine) {
            shelltools::print_task_header();
        }
        shelltools::print_task(*found, machine, 0, -1, detail);
    } else {
        if (!machine) {
            const auto processes =
                std::count_if(rows.begin(), rows.end(), [](const shelltools::TaskRow &row) {
                    return row.type == NT_PROCESS;
                });
            Printf("ShowTasks  %lu total  %lu processes  %lu tasks\n",
                   static_cast<ULONG>(snapshot.count), static_cast<ULONG>(processes),
                   static_cast<ULONG>(snapshot.count) - static_cast<ULONG>(processes));
            shelltools::print_task_header();
        }
        for (const shelltools::TaskRow &row : rows) {
            shelltools::print_task(row, machine, 0, -1, detail);
        }
    }
    if (snapshot.truncated) {
        PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
        return shelltools::warn;
    }
    return shelltools::ok;
}
