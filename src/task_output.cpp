#include "task_output.hpp"
#include "tool_support.hpp"

#include <exec/nodes.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace shelltools {

void print_task_header() {
    PutStr("TASK_ID  PROC_NO TYPE    STATE PRI NAME / COMMAND\n");
}

namespace {

void print_safe(std::string_view value) {
    std::array<char, 96> safe;
    unsigned n = 0;
    for (const char character : std::span{value.data(), std::min(value.size(), safe.size() - 1)}) {
        const UBYTE c = static_cast<UBYTE>(character);
        safe[n] = (c < 0x20 || (c >= 0x7f && c < 0xa0)) ? '?' : static_cast<char>(c);
        ++n;
    }
    safe[n] = '\0';
    PutStr(safe.data());
}

} // namespace

void print_task(const TaskRow &row, bool machine, LONG snapshot_number) {
    if (!machine) {
        Printf("%08lx %6ld %-7s %-5s %3ld ", row.address, row.cli,
               row.type == NT_PROCESS ? "PROCESS" : "TASK", task_state(row.state),
               static_cast<LONG>(row.priority));
        print_safe(row.name.data());
        PutStr(" / ");
        print_safe(row.command.data());
        PutStr("\n  ");
        Printf("FLAGS=%02lx SIGALLOC=%08lx SIGWAIT=%08lx SIGRECV=%08lx ",
               static_cast<ULONG>(row.flags), row.signal_alloc, row.signal_wait,
               row.signal_received);
        Printf("STACK=%08lx-%08lx SAVED_SP=%08lx\n", row.stack_lower, row.stack_upper,
               row.stack_pointer);
        if (row.type == NT_PROCESS) {
            Printf("  PROCESS_STACK=%ld PROCESS_FLAGS=%08lx ARGS=", row.process_stack_size,
                   static_cast<ULONG>(row.process_flags));
            print_safe(row.arguments.data());
            PutStr("\n");
        }
        return;
    }
    if (snapshot_number > 0) {
        Printf("{\"snapshot\":%ld,", snapshot_number);
    } else {
        PutStr("{");
    }
    Printf("\"task_id\":\"%08lx\",\"cli_id\":%ld,\"type\":\"%s\",\"state\":\"%s\",\"priority\":%ld,"
           "\"flags\":%lu,",
           row.address, row.cli, row.type == NT_PROCESS ? "process" : "task", task_state(row.state),
           static_cast<LONG>(row.priority), static_cast<ULONG>(row.flags));
    if (row.cli > 0) {
        Printf("\"process_number\":%ld,", row.cli);
    } else {
        PutStr("\"process_number\":null,");
    }
    Printf("\"signal_alloc\":%lu,\"signal_wait\":%lu,\"signal_received\":%lu,", row.signal_alloc,
           row.signal_wait, row.signal_received);
    Printf("\"stack_lower\":\"%08lx\",\"stack_upper\":\"%08lx\",\"saved_sp\":\"%08lx\",",
           row.stack_lower, row.stack_upper, row.stack_pointer);
    Printf("\"process_stack_size\":%ld,\"process_flags\":%ld,\"name\":", row.process_stack_size,
           row.process_flags);
    write_json_string(Output(), row.name.data());
    PutStr(",\"command\":");
    write_json_string(Output(), row.command.data());
    PutStr(",\"arguments\":");
    write_json_string(Output(), row.arguments.data());
    PutStr("}\n");
}

} // namespace shelltools
