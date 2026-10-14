#pragma once

#include <exec/types.h>

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
    char name[48];
    char command[64];
    char arguments[96];
};

struct TaskSnapshot {
    static constexpr unsigned capacity = 96;
    TaskRow rows[capacity];
    unsigned count;
    bool truncated;
};

void capture_tasks(TaskSnapshot &snapshot);
const char *task_state(UBYTE state);

} // namespace shelltools
