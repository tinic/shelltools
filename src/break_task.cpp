#include "tool_support.hpp"

#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <proto/exec.h>

namespace {

bool same_command(const Process *process, const char *expected) {
    if (process->pr_CLI == 0)
        return false;
    const auto *cli = static_cast<const CommandLineInterface *>(BADDR(process->pr_CLI));
    if (cli == nullptr || cli->cli_CommandName == 0)
        return false;
    const auto *bstr = static_cast<const UBYTE *>(BADDR(cli->cli_CommandName));
    if (bstr == nullptr)
        return false;
    unsigned i = 0;
    const unsigned length = bstr[0];
    while (i < length && expected[i] != '\0') {
        if (static_cast<UBYTE>(expected[i]) != bstr[i + 1])
            return false;
        ++i;
    }
    return i == length && expected[i] == '\0';
}

bool same_name(const Task *task, const char *expected) {
    const char *actual = task->tc_Node.ln_Name;
    if (actual == nullptr)
        return false;
    while (*actual && *expected && *actual == *expected) {
        ++actual;
        ++expected;
    }
    return *actual == '\0' && *expected == '\0';
}

bool parse_task_id(const char *text, ULONG &address) {
    if (text[0] == '$')
        ++text;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    if (*text == '\0')
        return false;
    address = 0;
    for (; *text; ++text) {
        unsigned digit;
        if (*text >= '0' && *text <= '9')
            digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f')
            digit = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F')
            digit = *text - 'A' + 10;
        else
            return false;
        if (address > 0x0fffffffUL)
            return false;
        address = (address << 4) | digit;
    }
    return address != 0;
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
    if (rd == nullptr)
        return shelltools::dos_error("BreakTask");
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
        (task_route && (args[2] == 0 || args[3] == 0 ||
                        !parse_task_id(reinterpret_cast<const char *>(args[2]), address)))) {
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
