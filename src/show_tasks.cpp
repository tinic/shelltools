#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <exec/nodes.h>
#include <proto/dos.h>

#include <algorithm>
#include <span>

int main() {
    LONG args[3] = {};
    RDArgs *rd = ReadArgs("MACHINE/S,DETAIL/S,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("ShowTasks");
    }
    if (args[2]) {
        shelltools::help("PURPOSE: List Exec tasks and AmigaDOS processes\n"
                         "USAGE: ShowTasks [DETAIL] [MACHINE]\n"
                         "DEFAULT: Aligned task table; P=process T=task\n"
                         "DETAIL: Signals, stack addresses, process fields\n"
                         "MACHINE: JSON lines with all fields\n"
                         "RC: 0=ok 5=truncated 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const bool machine = args[0] != 0;
    const bool detail = args[1] != 0;
    FreeArgs(rd);

    static shelltools::TaskSnapshot snapshot;
    shelltools::capture_tasks(snapshot);
    if (!machine) {
        const auto rows = std::span{snapshot.rows}.first(snapshot.count);
        const auto processes =
            std::count_if(rows.begin(), rows.end(),
                          [](const shelltools::TaskRow &row) { return row.type == NT_PROCESS; });
        Printf("ShowTasks  %lu total  %lu processes  %lu tasks\n",
               static_cast<ULONG>(snapshot.count), static_cast<ULONG>(processes),
               static_cast<ULONG>(snapshot.count) - static_cast<ULONG>(processes));
        shelltools::print_task_header();
    }
    for (unsigned i = 0; i < snapshot.count; ++i) {
        shelltools::print_task(snapshot.rows[i], machine, 0, -1, detail);
    }
    if (snapshot.truncated) {
        PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
        return shelltools::warn;
    }
    return shelltools::ok;
}
