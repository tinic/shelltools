#include "tool_support.hpp"

#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include <algorithm>
#include <span>
#include <string_view>

namespace {

bool same_command(const Process *process, std::string_view expected) {
    if (process->pr_CLI == 0) {
        return false;
    }
    const auto *cli = static_cast<const CommandLineInterface *>(BADDR(process->pr_CLI));
    if (cli == nullptr || cli->cli_CommandName == 0) {
        return false;
    }
    const auto *bstr = static_cast<const UBYTE *>(BADDR(cli->cli_CommandName));
    if (bstr == nullptr) {
        return false;
    }
    const auto actual = std::span{bstr + 1, static_cast<unsigned>(bstr[0])};
    return std::ranges::equal(actual, expected,
                              [](UBYTE a, char b) { return a == static_cast<UBYTE>(b); });
}

constexpr bool same_name(const Task *task, std::string_view expected) {
    const char *actual = task->tc_Node.ln_Name;
    return actual != nullptr && std::string_view{actual} == expected;
}

bool signal_matching(const List &list, ULONG address, const char *name) {
    for (const Node *node = list.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        Task *task = reinterpret_cast<Task *>(const_cast<Node *>(node));
        if (reinterpret_cast<ULONG>(task) == address && same_name(task, name)) {
            Signal(task, SIGBREAKF_CTRL_C);
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    LONG args[5] = {};
    RDArgs *rd = ReadArgs("CLI/N,COMMAND,TASK,NAME,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("BreakTask");
    }
    if (args[4]) {
        shelltools::help("PURPOSE: Send Ctrl-C break to an Exec task\n"
                         "USAGE: BreakTask CLI=<number> COMMAND=<exact name>\n"
                         "   OR: BreakTask TASK=<hex task ID> NAME=<exact task name>\n"
                         "RC: 0=signaled 5=changed/missing 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const bool cli_route = args[0] != 0 || args[1] != 0;
    const bool task_route = args[2] != 0 || args[3] != 0;
    ULONG address = 0;
    if (cli_route == task_route ||
        (cli_route && (args[0] == 0 || args[1] == 0 || *reinterpret_cast<LONG *>(args[0]) <= 0)) ||
        (task_route &&
         (args[2] == 0 || args[3] == 0 ||
          !shelltools::parse_task_id(reinterpret_cast<const char *>(args[2]), address)))) {
        PutStr("BreakTask: use CLI+COMMAND or TASK+NAME; see HELP\n");
        FreeArgs(rd);
        return shelltools::error;
    }

    // Resolve and signal within one critical section so the Process pointer
    // cannot be freed between lookup and Signal().
    bool sent = false;
    Forbid();
    if (cli_route) {
        const ULONG cli_number = static_cast<ULONG>(*reinterpret_cast<LONG *>(args[0]));
        Process *process = FindCliProc(cli_number);
        if (process != nullptr && same_command(process, reinterpret_cast<const char *>(args[1]))) {
            Signal(&process->pr_Task, SIGBREAKF_CTRL_C);
            sent = true;
        }
    } else {
        const char *name = reinterpret_cast<const char *>(args[3]);
        Task *current = SysBase->ThisTask;
        if (reinterpret_cast<ULONG>(current) == address && same_name(current, name)) {
            Signal(current, SIGBREAKF_CTRL_C);
            sent = true;
        } else {
            sent = signal_matching(SysBase->TaskReady, address, name) ||
                   signal_matching(SysBase->TaskWait, address, name);
        }
    }
    Permit();
    FreeArgs(rd);
    if (!sent) {
        PutStr("BreakTask: task missing or identity changed\n");
        return shelltools::warn;
    }
    PutStr("SIGNAL=CTRL_C\n");
    return shelltools::ok;
}
