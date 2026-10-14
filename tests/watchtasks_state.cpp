#include <exec/execbase.h>
#include <proto/exec.h>

#include <cstdio>
#include <cstring>

namespace {
unsigned signal_calls = 0;
unsigned forbid_depth = 0;
Task *signalled_task = nullptr;
ULONG signalled_mask = 0;
bool signal_protected = true;
void test_forbid() {
    ++forbid_depth;
}
void test_permit() {
    --forbid_depth;
}
void test_signal(Task *task, ULONG mask) {
    ++signal_calls;
    signalled_task = task;
    signalled_mask = mask;
    signal_protected &= forbid_depth == 1;
}
} // namespace

// Exercise the production state machine and live identity check, replacing
// only Exec signalling/scheduling so no real task receives a break signal.
#undef Forbid
#undef Permit
#undef Signal
#define Forbid() test_forbid()
#define Permit() test_permit()
#define Signal(task, mask) test_signal(task, mask)
#define main watchtasks_program_main
#include "../src/watch_tasks.cpp"
#undef main
#undef Signal
#undef Permit
#undef Forbid

#define CHECK(test)                                                                                \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            SysBase = saved;                                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #test);                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    ExecBase *saved = SysBase;
    static ExecBase exec = {};
    static Task first = {};
    static Task second = {};
    first.tc_Node.ln_Type = NT_PROCESS;
    first.tc_Node.ln_Name = const_cast<char *>("first");
    second.tc_Node.ln_Type = NT_PROCESS;
    second.tc_Node.ln_Name = const_cast<char *>("second");
    exec.ThisTask = &first;
    exec.TaskReady.lh_Head = reinterpret_cast<Node *>(&exec.TaskReady.lh_Tail);
    exec.TaskWait.lh_Head = reinterpret_cast<Node *>(&exec.TaskWait.lh_Tail);
    SysBase = &exec;

    static View view;
    static TaskRow a = {};
    static TaskRow b = {};
    a.address = reinterpret_cast<ULONG>(&first);
    a.type = NT_PROCESS;
    std::memcpy(a.name.data(), "first", 6);
    std::memcpy(a.command.data(), "first-command", 14);
    b.address = reinterpret_cast<ULONG>(&second);
    b.type = NT_PROCESS;
    std::memcpy(b.name.data(), "second", 7);
    view.snapshot.count = 2;
    view.snapshot.rows[0] = a;
    view.snapshot.rows[1] = b;
    view.cpu.tenths[0] = 900;
    prepare(view);
    CHECK(view.selected_task.address == a.address);

    // The capture array and CPU ordering both change; selection follows identity.
    view.snapshot.rows[0] = b;
    view.snapshot.rows[1] = a;
    view.cpu.tenths[0] = 900;
    view.cpu.tenths[1] = 0;
    prepare(view);
    CHECK(view.selected == 1 && view.selected_task.address == a.address);
    bool done = false;
    handle_key(view, 'j', done);
    prepare(view);
    handle_key(view, 'k', done);
    prepare(view);
    CHECK(view.selected_task.address == b.address);
    handle_key(view, 'j', done);
    prepare(view);
    CHECK(view.selected_task.address == a.address);

    for (char cancel : std::array{'n', 'N', '\r', '\n', '\x1b', 'q', 'Q'}) {
        handle_key(view, 'b', done);
        CHECK(view.confirming_break && signal_calls == 0);
        handle_key(view, 'b', done);
        handle_key(view, 'j', done);
        CHECK(view.confirming_break && view.break_target.address == a.address);
        handle_key(view, cancel, done);
        CHECK(!view.confirming_break && signal_calls == 0 && !done);
    }

    handle_key(view, 'b', done);
    // Even a selection change while the prompt is open cannot retarget the break.
    view.selection_valid = false;
    view.selected = 0;
    prepare(view);
    CHECK(view.selected_task.address == b.address);
    handle_key(view, 'Y', done);
    CHECK(signal_calls == 1 && signalled_task == &first);
    CHECK(signalled_mask == SIGBREAKF_CTRL_C && signal_protected && forbid_depth == 0);

    // A disappeared or renamed task is not replaced by another live task.
    view.selection_valid = false;
    view.selected = 1;
    prepare(view);
    handle_key(view, 'b', done);
    exec.ThisTask = &second;
    handle_key(view, 'y', done);
    CHECK(signal_calls == 1 && !view.confirming_break);
    CHECK(std::string_view{view.message} == "Task changed or missing");
    exec.ThisTask = &first;
    handle_key(view, 'b', done);
    first.tc_Node.ln_Name = const_cast<char *>("renamed");
    handle_key(view, 'y', done);
    CHECK(signal_calls == 1);
    first.tc_Node.ln_Name = const_cast<char *>("first");

    // Once absent in a refreshed capture, even a reused identity stays refused.
    handle_key(view, 'b', done);
    view.snapshot.count = 1;
    view.snapshot.rows[0] = b;
    prepare(view);
    CHECK(view.break_target_missing);
    static Panel missing_panel;
    render_break_confirmation(view, missing_panel);
    const Line &missing_footer = missing_panel.rows[missing_panel.height - 1];
    CHECK(std::string_view(missing_footer.data.data(), missing_footer.length).find("Y send") ==
          std::string_view::npos);
    view.snapshot.count = 2;
    view.snapshot.rows[0] = b;
    view.snapshot.rows[1] = a;
    prepare(view);
    CHECK(view.break_target_missing);
    handle_key(view, 'Y', done);
    CHECK(signal_calls == 1);

    view.selection_valid = false;
    view.selected = 1;
    prepare(view);
    handle_key(view, 'b', done);
    view.snapshot.rows[1].command[0] = 'X';
    prepare(view);
    CHECK(view.break_target_missing);
    handle_key(view, 'Y', done);
    CHECK(signal_calls == 1);
    view.snapshot.rows[1] = a;
    view.selection_valid = false;
    view.selected = 1;
    prepare(view);

    // The smallest supported window still exposes identity and cancel controls.
    handle_key(view, 'b', done);
    view.rows = 14;
    view.cols = 40;
    static Panel panel;
    render_break_confirmation(view, panel);
    CHECK(panel.top > 1 && panel.top + panel.height <= view.rows);
    CHECK(std::string_view(panel.rows[3].data.data(), panel.rows[3].length).find("first") !=
          std::string_view::npos);
    CHECK(std::string_view(panel.rows[4].data.data(), panel.rows[4].length).find("first-command") !=
          std::string_view::npos);
    const Line &footer = panel.rows[panel.height - 1];
    CHECK(std::string_view(footer.data.data(), footer.length).find("cancel") !=
          std::string_view::npos);
    handle_key(view, '\r', done);
    CHECK(signal_calls == 1 && forbid_depth == 0);
    SysBase = saved;
    std::puts("PASS: stable selection, explicit confirmation, cancellation, pinned target, "
              "missing/renamed task, 40x14 prompt");
    return 0;
}
