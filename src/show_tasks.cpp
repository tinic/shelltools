#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <proto/dos.h>

int main() {
    LONG args[2] = {};
    RDArgs *rd = ReadArgs("MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("ShowTasks");
    if (args[1]) {
        shelltools::help("PURPOSE: List Exec tasks and AmigaDOS processes\n"
                         "USAGE: ShowTasks [MACHINE]\n"
                         "FIELDS: task ID, process number, type, state, priority, signals, stack\n"
                         "OUTPUT: Detail rows; MACHINE gives JSON lines\n"
                         "RC: 0=ok 5=truncated 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const bool machine = args[0] != 0;
    FreeArgs(rd);

    static shelltools::TaskSnapshot snapshot;
    shelltools::capture_tasks(snapshot);
    if (!machine)
        shelltools::print_task_header();
    for (unsigned i = 0; i < snapshot.count; ++i) {
        shelltools::print_task(snapshot.rows[i], machine, 0);
    }
    if (snapshot.truncated) {
        PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
        return shelltools::warn;
    }
    return shelltools::ok;
}
