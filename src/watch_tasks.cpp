#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <dos/dosextens.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <proto/exec.h>

namespace {

using shelltools::TaskRow;
using shelltools::TaskSnapshot;

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

int compare_text(const char *a, const char *b) {
    while (*a && *b) {
        const char x = lower(*a++);
        const char y = lower(*b++);
        if (x != y)
            return static_cast<unsigned char>(x) - static_cast<unsigned char>(y);
    }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

bool contains(const char *text, const char *part) {
    if (!*part)
        return true;
    for (; *text; ++text) {
        const char *a = text;
        const char *b = part;
        while (*a && *b && lower(*a) == lower(*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

void safe_clip(char *out, unsigned capacity, const char *text) {
    unsigned i = 0;
    while (text[i] && i + 1 < capacity) {
        const UBYTE c = static_cast<UBYTE>(text[i]);
        out[i] = (c < 0x20 || (c >= 0x7f && c < 0xa0)) ? '?' : static_cast<char>(c);
        ++i;
    }
    out[i] = '\0';
}

struct View {
    TaskSnapshot snapshot;
    unsigned order[TaskSnapshot::capacity];
    unsigned shown = 0;
    unsigned selected = 0;
    unsigned scroll = 0;
    unsigned rows = 24;
    char filter[32] = {};
    const char *message = "";
    char sort = 'p';
    bool paused = false;
    bool help = false;
    bool editing = false;
    bool truncated = false;
};

bool before(const TaskRow &a, const TaskRow &b, char sort) {
    if (sort == 'p' && a.priority != b.priority)
        return a.priority > b.priority;
    if (sort == 's' && a.state != b.state)
        return a.state < b.state;
    if (sort == 'c' && a.cli != b.cli)
        return a.cli < b.cli;
    const int names = compare_text(a.name, b.name);
    return names ? names < 0 : a.address < b.address;
}

void prepare(View &view) {
    view.shown = 0;
    for (unsigned i = 0; i < view.snapshot.count; ++i) {
        const TaskRow &row = view.snapshot.rows[i];
        if (!contains(row.name, view.filter) && !contains(row.command, view.filter))
            continue;
        unsigned at = view.shown++;
        while (at && before(row, view.snapshot.rows[view.order[at - 1]], view.sort)) {
            view.order[at] = view.order[at - 1];
            --at;
        }
        view.order[at] = i;
    }
    if (view.selected >= view.shown)
        view.selected = view.shown ? view.shown - 1 : 0;
    const unsigned page = view.rows - 11;
    if (view.selected < view.scroll)
        view.scroll = view.selected;
    if (view.selected >= view.scroll + page)
        view.scroll = view.selected - page + 1;
}

void memory_bar(const char *label, ULONG free, ULONG total) {
    char bar[23];
    bar[0] = '\253'; // ISO-8859-1 left guillemet
    const ULONG used = total > free ? total - free : 0;
    const ULONG step = total / 20 + (total % 20 != 0);
    unsigned filled = step ? used / step : 0;
    if (filled > 20)
        filled = 20;
    for (unsigned i = 0; i < 20; ++i)
        bar[i + 1] = i < filled ? '#' : '\267';
    bar[21] = '\273'; // ISO-8859-1 right guillemet
    bar[22] = '\0';
    Printf("%-4s %s %6luK / %6luK\n", label, bar, used / 1024, total / 1024);
}

bool same_name(const char *live, const char *saved) {
    if (live == nullptr)
        return false;
    while (*live && *saved && *live == *saved) {
        ++live;
        ++saved;
    }
    return *live == '\0' && *saved == '\0';
}

bool signal_matching(const List &list, const TaskRow &row) {
    for (const Node *node = list.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        Task *task = reinterpret_cast<Task *>(const_cast<Node *>(node));
        if (reinterpret_cast<ULONG>(task) == row.address && task->tc_Node.ln_Type == row.type &&
            same_name(task->tc_Node.ln_Name, row.name)) {
            Signal(task, SIGBREAKF_CTRL_C);
            return true;
        }
    }
    return false;
}

bool break_selected(const TaskRow &row) {
    bool sent = false;
    Forbid();
    Task *current = SysBase->ThisTask;
    if (reinterpret_cast<ULONG>(current) == row.address && current->tc_Node.ln_Type == row.type &&
        same_name(current->tc_Node.ln_Name, row.name)) {
        Signal(current, SIGBREAKF_CTRL_C);
        sent = true;
    } else
        sent = signal_matching(SysBase->TaskReady, row) || signal_matching(SysBase->TaskWait, row);
    Permit();
    return sent;
}

void render(View &view, LONG number) {
    prepare(view);
    unsigned running = 0, ready = 0, waiting = 0;
    for (unsigned i = 0; i < view.snapshot.count; ++i) {
        const UBYTE state = view.snapshot.rows[i].state;
        if (state == TS_RUN)
            ++running;
        else if (state == TS_READY)
            ++ready;
        else if (state == TS_WAIT)
            ++waiting;
    }
    PutStr("\233H\233J\2330 p\2337m WatchTasks ");
    Printf(" snapshot %ld  tasks %lu  sort %lc %s %s\2330m\n", number,
           static_cast<ULONG>(view.snapshot.count), static_cast<LONG>(view.sort),
           view.paused ? "PAUSED" : "", view.snapshot.truncated ? "TRUNCATED" : "");
    memory_bar("CHIP", AvailMem(MEMF_CHIP), AvailMem(MEMF_CHIP | MEMF_TOTAL));
    memory_bar("FAST", AvailMem(MEMF_FAST), AvailMem(MEMF_FAST | MEMF_TOTAL));
    Printf("RUN %lu  READY %lu  WAIT %lu  CPU unavailable\n", static_cast<ULONG>(running),
           static_cast<ULONG>(ready), static_cast<ULONG>(waiting));
    PutStr("-------------------------------------------------------------------------------\n");
    if (view.help) {
        PutStr("j/k or arrows  select       n/u  page down/up\n"
               "p/s/c/a        sort by priority/state/CLI/name\n"
               "/              filter name or command; Enter accepts\n"
               "b              send Ctrl-C to selected task\n"
               "Space          pause/resume snapshots\n"
               "h              close help     q  quit\n");
    } else {
        PutStr("  TASK_ID   CLI  PRI STATE NAME               COMMAND\n");
        const unsigned page = view.rows - 11;
        for (unsigned line = 0; line < page; ++line) {
            const unsigned pos = view.scroll + line;
            if (pos >= view.shown) {
                PutStr("\233K\n");
                continue;
            }
            const TaskRow &row = view.snapshot.rows[view.order[pos]];
            char name[21], command[29];
            safe_clip(name, sizeof name, row.name);
            safe_clip(command, sizeof command, row.command);
            if (pos == view.selected)
                PutStr("\2337m");
            Printf("%lc %08lx %5ld %4ld %-5s %-20s %-28s", pos == view.selected ? '>' : ' ',
                   row.address, row.cli, static_cast<LONG>(row.priority),
                   shelltools::task_state(row.state), name, command);
            if (pos == view.selected)
                PutStr("\2330m");
            PutStr("\233K\n");
        }
        if (view.shown) {
            const TaskRow &row = view.snapshot.rows[view.order[view.selected]];
            Printf("\233KSELECT %lu/%lu  TYPE %s  TASK %08lx  CLI %ld\n",
                   static_cast<ULONG>(view.selected + 1), static_cast<ULONG>(view.shown),
                   row.type == NT_PROCESS ? "PROCESS" : "TASK", row.address, row.cli);
            Printf("\233KSIGNALS alloc %08lx wait %08lx recv %08lx  STACK %08lx-%08lx\n",
                   row.signal_alloc, row.signal_wait, row.signal_received, row.stack_lower,
                   row.stack_upper);
        } else
            PutStr("\233KNo matching tasks\n\233K\n");
    }
    if (view.editing)
        Printf("\233KFilter: %s_\n", view.filter);
    else
        Printf("\233KFilter: %s  %s\n", view.filter, view.message);
    PutStr("\233Kj/k move  p/s/c/a sort  / filter  b break  h help  Space pause  q quit\233K");
}

struct RawConsole {
    BPTR input;
    bool active;
    explicit RawConsole(BPTR in) : input(in), active(SetMode(in, 1) != 0) {}
    ~RawConsole() {
        if (active) {
            PutStr("\2330m\233 p\233J\n");
            SetMode(input, 0);
        }
    }
};

int watch_snapshots(LONG ticks, LONG count, bool machine);

void handle_key(View &view, char key, bool &done) {
    if (view.editing) {
        unsigned len = 0;
        while (view.filter[len])
            ++len;
        if (key == '\r' || key == '\n' || key == 27)
            view.editing = false;
        else if ((key == 8 || key == 127) && len)
            view.filter[len - 1] = '\0';
        else if (static_cast<UBYTE>(key) >= 32 && static_cast<UBYTE>(key) < 127 &&
                 len + 1 < sizeof view.filter) {
            view.filter[len] = key;
            view.filter[len + 1] = '\0';
        }
        view.selected = 0;
        view.scroll = 0;
        return;
    }
    if (key == 'q' || key == 'Q')
        done = true;
    else if (key == 'h' || key == 'H' || key == '?')
        view.help = !view.help;
    else if (key == ' ')
        view.paused = !view.paused;
    else if (key == '/')
        view.editing = true;
    else if (key == 'j') {
        if (view.selected + 1 < view.shown)
            ++view.selected;
    } else if (key == 'k') {
        if (view.selected)
            --view.selected;
    } else if (key == 'n')
        view.selected += view.rows - 11;
    else if (key == 'u')
        view.selected = view.selected > view.rows - 11 ? view.selected - (view.rows - 11) : 0;
    else if (key == 'p' || key == 's' || key == 'c' || key == 'a') {
        view.sort = key;
        view.selected = 0;
        view.scroll = 0;
    } else if (key == 'b') {
        const bool sent =
            view.shown && break_selected(view.snapshot.rows[view.order[view.selected]]);
        view.message = sent ? "Ctrl-C sent" : "Task changed or missing";
    }
}

int watch_interactive(LONG ticks, LONG count, unsigned rows) {
    RawConsole console(Input());
    if (!console.active)
        return watch_snapshots(ticks, count, false);
    static View view;
    view.rows = rows;
    LONG number = 0;
    bool done = false;
    unsigned escape = 0;
    while (!done && (count == 0 || number < count)) {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            return shelltools::warn;
        if (!view.paused || number == 0) {
            shelltools::capture_tasks(view.snapshot);
            view.truncated |= view.snapshot.truncated;
            ++number;
        }
        render(view, number);
        for (LONG step = 0; step < ticks && !done; ++step) {
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
                return shelltools::warn;
            if (WaitForChar(Input(), 20000)) {
                char key = 0;
                if (Read(Input(), &key, 1) != 1)
                    continue;
                if (key == 27 && view.editing) {
                    view.editing = false;
                    render(view, number);
                    continue;
                }
                if (key == '\233' || key == 27) {
                    escape = key == 27 ? 1 : 2;
                    continue;
                }
                if (escape == 1 && key == '[') {
                    escape = 2;
                    continue;
                }
                if (escape == 2 && key != 'A' && key != 'B') {
                    escape = 0;
                    continue;
                }
                if (escape == 2)
                    key = key == 'A' ? 'k' : 'j';
                escape = 0;
                handle_key(view, key, done);
                render(view, number);
            }
        }
    }
    return view.truncated ? shelltools::warn : shelltools::ok;
}

int watch_snapshots(LONG ticks, LONG count, bool machine) {
    static TaskSnapshot snapshot;
    for (LONG round = 0; count == 0 || round < count; ++round) {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            return shelltools::warn;
        shelltools::capture_tasks(snapshot);
        if (machine) {
            Printf("{\"snapshot\":%ld,\"chip_free\":%lu,\"fast_free\":%lu}\n", round + 1,
                   AvailMem(MEMF_CHIP), AvailMem(MEMF_FAST));
        } else {
            Printf("SNAPSHOT=%ld CHIP_FREE=%lu FAST_FREE=%lu\n", round + 1, AvailMem(MEMF_CHIP),
                   AvailMem(MEMF_FAST));
            shelltools::print_task_header();
        }
        for (unsigned i = 0; i < snapshot.count; ++i) {
            shelltools::print_task(snapshot.rows[i], machine, round + 1);
        }
        if (snapshot.truncated) {
            PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
            return shelltools::warn;
        }
        if (count == 0 || round + 1 < count)
            Delay(ticks);
    }
    return shelltools::ok;
}

} // namespace

int main() {
    LONG args[5] = {};
    RDArgs *rd = ReadArgs("TICKS/N/K,COUNT/N/K,ROWS/N/K,MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("WatchTasks");
    if (args[4]) {
        shelltools::help("PURPOSE: Live task and memory monitor\n"
                         "USAGE: WatchTasks [TICKS=<n>] [COUNT=<n>] [ROWS=<n>] [MACHINE]\n"
                         "DEFAULT: 50 ticks, unlimited, 24 rows\n"
                         "KEYS: j/k move; p/s/c/a sort; / filter; b break; h help; q quit\n"
                         "OUTPUT: Interactive ISO-8859-1 UI; MACHINE gives JSON lines\n"
                         "RC: 0=done 5=break/truncated 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const LONG ticks = args[0] ? *reinterpret_cast<LONG *>(args[0]) : 50;
    const LONG count = args[1] ? *reinterpret_cast<LONG *>(args[1]) : 0;
    const LONG rows = args[2] ? *reinterpret_cast<LONG *>(args[2]) : 24;
    const bool machine = args[3] != 0;
    if (ticks <= 0 || count < 0 || rows < 14 || rows > 100) {
        PutStr("WatchTasks: TICKS>0 COUNT>=0 ROWS=14..100\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    FreeArgs(rd);
    if (!machine && IsInteractive(Input()) && IsInteractive(Output()))
        return watch_interactive(ticks, count, static_cast<unsigned>(rows));
    return watch_snapshots(ticks, count, machine);
}
