#include "task_snapshot.hpp"

#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include <algorithm>
#include <span>

namespace shelltools {

namespace {

void copy_name(std::span<char> dest, const char *src) {
    if (src == nullptr) {
        dest.front() = '\0';
        return;
    }
    unsigned length = 0;
    for (const char c : std::span{src, dest.size() - 1}) {
        if (c == '\0') {
            break;
        }
        dest[length++] = c;
    }
    dest[length] = '\0';
}

void copy_command(std::span<char> dest, const Process *process) {
    dest.front() = '\0';
    if (process->pr_CLI == 0) {
        return;
    }
    const auto *cli = static_cast<const CommandLineInterface *>(BADDR(process->pr_CLI));
    if (cli == nullptr || cli->cli_CommandName == 0) {
        return;
    }
    const auto *bstr = static_cast<const UBYTE *>(BADDR(cli->cli_CommandName));
    if (bstr == nullptr) {
        return;
    }
    const auto length = std::min<unsigned>(bstr[0], dest.size() - 1);
    const auto source = std::span{bstr + 1, length};
    std::ranges::transform(source, dest.begin(),
                           [](UBYTE value) { return static_cast<char>(value); });
    dest[length] = '\0';
}

void capture_one(TaskSnapshot &snapshot, const Task *task) {
    if (snapshot.count == TaskSnapshot::capacity) {
        snapshot.truncated = true;
        return;
    }
    TaskRow &row = snapshot.rows[snapshot.count++];
    row.address = reinterpret_cast<ULONG>(task);
    row.priority = task->tc_Node.ln_Pri;
    row.state = task->tc_State;
    row.type = task->tc_Node.ln_Type;
    row.flags = task->tc_Flags;
    row.signal_alloc = task->tc_SigAlloc;
    row.signal_wait = task->tc_SigWait;
    row.signal_received = task->tc_SigRecvd;
    row.stack_lower = reinterpret_cast<ULONG>(task->tc_SPLower);
    row.stack_upper = reinterpret_cast<ULONG>(task->tc_SPUpper);
    row.stack_pointer = reinterpret_cast<ULONG>(task->tc_SPReg);
    row.cli = 0;
    row.process_stack_size = 0;
    row.process_flags = 0;
    row.command.front() = '\0';
    row.arguments.front() = '\0';
    copy_name(row.name, task->tc_Node.ln_Name);
    if (task->tc_Node.ln_Type == NT_PROCESS) {
        const auto *process = reinterpret_cast<const Process *>(task);
        row.cli = process->pr_TaskNum;
        row.process_stack_size = process->pr_StackSize;
        row.process_flags = process->pr_Flags;
        copy_command(row.command, process);
        copy_name(row.arguments, process->pr_Arguments);
    }
}

void capture_list(TaskSnapshot &snapshot, const List &list) {
    for (const Node *node = list.lh_Head; node->ln_Succ != nullptr; node = node->ln_Succ) {
        capture_one(snapshot, reinterpret_cast<const Task *>(node));
    }
}

} // namespace

void capture_tasks(TaskSnapshot &snapshot) {
    snapshot.count = 0;
    snapshot.truncated = false;
    // No allocation, I/O or calls that can Wait() while holding Forbid().
    Forbid();
    capture_one(snapshot, SysBase->ThisTask);
    capture_list(snapshot, SysBase->TaskReady);
    capture_list(snapshot, SysBase->TaskWait);
    Permit();
}

const char *task_state(UBYTE state) {
    switch (state) {
    case TS_RUN:
        return "RUN";
    case TS_READY:
        return "READY";
    case TS_WAIT:
        return "WAIT";
    case TS_EXCEPT:
        return "EXCEPT";
    default:
        return "OTHER";
    }
}

} // namespace shelltools
