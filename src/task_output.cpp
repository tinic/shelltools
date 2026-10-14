#include "task_output.hpp"
#include "tool_support.hpp"

#include <exec/nodes.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace shelltools {

void print_task_header(bool show_cpu) {
    PutStr(show_cpu ? "TYPE TASK_ID   CLI  PRI STATE  CPU% STACK_B NAME\n"
                    : "TYPE TASK_ID   CLI  PRI STATE  STACK_B NAME\n");
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

void print_task(const TaskRow &row, bool machine, LONG snapshot_number, LONG cpu_tenths,
                bool detail) {
    if (!machine) {
        const ULONG stack_bytes =
            row.stack_upper >= row.stack_lower ? row.stack_upper - row.stack_lower : 0;
        PutStr(row.type == NT_PROCESS ? "P    " : "T    ");
        Printf("%08lx %5ld %4ld %-6s ", row.address, row.cli, static_cast<LONG>(row.priority),
               task_state(row.state));
        if (snapshot_number > 0) {
            if (cpu_tenths >= 0) {
                Printf("%3ld.%ld ", cpu_tenths / 10, cpu_tenths % 10);
            } else {
                PutStr(" --.- ");
            }
        }
        Printf("%7lu ", stack_bytes);
        print_safe(row.name.data());
        PutStr("\n");
        if (row.command.front() != '\0' &&
            std::string_view{row.command.data()} != std::string_view{row.name.data()}) {
            PutStr("  CMD       ");
            print_safe(row.command.data());
            PutStr("\n");
        }
        if (!detail) {
            return;
        }
        Printf("  SIGNALS   ALLOC %08lx  WAIT %08lx  RECV %08lx  FLAGS %02lx\n", row.signal_alloc,
               row.signal_wait, row.signal_received, static_cast<ULONG>(row.flags));
        Printf("  STACK     %08lx-%08lx  SAVED %08lx\n", row.stack_lower, row.stack_upper,
               row.stack_pointer);
        if (row.type == NT_PROCESS) {
            Printf("  PROCESS   STACK %ld  FLAGS %08lx\n", row.process_stack_size,
                   static_cast<ULONG>(row.process_flags));
            if (row.arguments.front() != '\0') {
                PutStr("  ARGS      ");
                print_safe(row.arguments.data());
                PutStr("\n");
            }
        }
        PutStr("\n");
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
    if (snapshot_number > 0) {
        if (cpu_tenths >= 0) {
            Printf("\"cpu_tenths\":%ld,", cpu_tenths);
        } else {
            PutStr("\"cpu_tenths\":null,");
        }
    }
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
