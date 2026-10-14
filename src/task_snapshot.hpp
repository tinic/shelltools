#pragma once

#include <exec/types.h>

#include <array>

namespace shelltools {

struct TaskRow {
    ULONG address;
    LONG cli;
    BYTE priority;
    UBYTE state;
    UBYTE type;
    UBYTE flags;
    ULONG signal_alloc;
    ULONG signal_wait;
    ULONG signal_received;
    ULONG stack_lower;
    ULONG stack_upper;
    ULONG stack_pointer;
    LONG process_stack_size;
    LONG process_flags;
    // capture_one fills these before a row is published.
    // cppcheck-suppress uninitMemberVarNoCtor
    std::array<char, 96> name;
    // cppcheck-suppress uninitMemberVarNoCtor
    std::array<char, 64> command;
    // cppcheck-suppress uninitMemberVarNoCtor
    std::array<char, 96> arguments;
};

struct TaskSnapshot {
    static constexpr unsigned capacity = 96;
    std::array<TaskRow, capacity> rows;
    // capture_tasks resets these before reading either value.
    // cppcheck-suppress uninitMemberVarNoCtor
    unsigned count;
    // cppcheck-suppress uninitMemberVarNoCtor
    bool truncated;
};

void capture_tasks(TaskSnapshot &snapshot);
const char *task_state(UBYTE state);

} // namespace shelltools
