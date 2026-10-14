#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <dos/dosextens.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace {

using shelltools::TaskRow;
using shelltools::TaskSnapshot;

struct CpuSampler {
    static constexpr unsigned capacity = 256;
    struct Buffer {
        std::array<Task *, capacity> tasks = {};
        unsigned next = 0;
        unsigned count = 0;
    };
    std::array<Buffer, 2> buffers = {};
    Interrupt server = {};
    unsigned active = 0;

    CpuSampler();
    ~CpuSampler();
    CpuSampler(const CpuSampler &) = delete;
    CpuSampler &operator=(const CpuSampler &) = delete;

    void sample() {
        Buffer &buffer = buffers[active];
        Task *task = SysBase->ThisTask;
        buffer.tasks[buffer.next] = task && task->tc_State == TS_RUN ? task : nullptr;
        buffer.next = (buffer.next + 1) % capacity;
        if (buffer.count < capacity) {
            ++buffer.count;
        }
    }

    unsigned take(std::array<Task *, capacity> &out) {
        Disable();
        const unsigned completed = active;
        active ^= 1;
        buffers[active].next = 0;
        buffers[active].count = 0;
        Enable();
        const Buffer &buffer = buffers[completed];
        std::ranges::copy(std::span{buffer.tasks}.first(buffer.count), out.begin());
        return buffer.count;
    }
};

CpuSampler *current_sampler = nullptr;

extern "C" __attribute__((noinline, used)) ULONG watchtasks_vblank() {
    if (current_sampler) {
        current_sampler->sample();
    }
    return 0; // Exec requires Z set so the next VBlank server runs.
}

inline CpuSampler::CpuSampler() {
    server.is_Node.ln_Type = NT_INTERRUPT;
    server.is_Node.ln_Pri = -60;
    server.is_Node.ln_Name = const_cast<char *>("WatchTasks CPU");
    server.is_Code = reinterpret_cast<decltype(server.is_Code)>(&watchtasks_vblank);
    current_sampler = this;
    AddIntServer(INTB_VERTB, &server);
}

inline CpuSampler::~CpuSampler() {
    RemIntServer(INTB_VERTB, &server);
    current_sampler = nullptr;
}

struct CpuUsage {
    static constexpr unsigned minimum_samples = 10;
    std::array<unsigned, TaskSnapshot::capacity> tenths = {};
    unsigned samples = 0;
    unsigned busy_tenths = 0;

    void update(CpuSampler &sampler, const TaskSnapshot &snapshot) {
        static std::array<Task *, CpuSampler::capacity> captured;
        samples = sampler.take(captured);
        tenths.fill(0);
        busy_tenths = 0;
        if (samples < minimum_samples) {
            samples = 0;
            return;
        }
        std::array<unsigned, TaskSnapshot::capacity> counts = {};
        for (Task *task : std::span{captured}.first(samples)) {
            if (task) {
                ++busy_tenths;
            }
            const ULONG address = reinterpret_cast<ULONG>(task);
            for (unsigned i = 0; i < snapshot.count; ++i) {
                if (snapshot.rows[i].address == address) {
                    ++counts[i];
                    break;
                }
            }
        }
        for (unsigned i = 0; i < snapshot.count; ++i) {
            tenths[i] = (counts[i] * 1000 + samples / 2) / samples;
        }
        busy_tenths = (busy_tenths * 1000 + samples / 2) / samples;
    }
};

constexpr char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr int compare_text(std::string_view a, std::string_view b) {
    const auto equal = [](char x, char y) { return lower(x) == lower(y); };
    const auto [left, right] = std::ranges::mismatch(a, b, equal);
    if (left == a.end()) {
        return right == b.end() ? 0 : -1;
    }
    if (right == b.end()) {
        return 1;
    }
    return static_cast<unsigned char>(lower(*left)) - static_cast<unsigned char>(lower(*right));
}

constexpr bool contains(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    const auto equal = [](char x, char y) { return lower(x) == lower(y); };
    return std::ranges::search(text, part, equal).begin() != text.end();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
struct View {
    // Static storage zero-initializes these large buffers without file data.
    // cppcheck-suppress uninitMemberVarNoCtor
    TaskSnapshot snapshot;
    CpuUsage cpu;
    // cppcheck-suppress uninitMemberVarNoCtor
    std::array<unsigned, TaskSnapshot::capacity> order;
    unsigned shown = 0;
    unsigned selected = 0;
    unsigned scroll = 0;
    unsigned rows = 24;
    unsigned cols = 80;
    std::array<char, 32> filter = {};
    const char *message = "";
    char sort = 't';
    bool paused = false;
    bool help = false;
    bool editing = false;
    bool truncated = false;
    ULONG chip_free = 0;
    ULONG chip_total = 0;
    ULONG fast_free = 0;
    ULONG fast_total = 0;
};
#pragma GCC diagnostic pop

constexpr unsigned max_rows = 100;
constexpr unsigned max_cols = 160;

struct Line {
    std::array<char, max_cols> data = {};
    unsigned length = 0;
    unsigned limit;

    constexpr explicit Line(unsigned cols, bool full_width = false)
        : limit(full_width ? std::min(cols, max_cols)
                           : (cols > 1 ? std::min(cols - 1, max_cols - 1) : 0)) {}

    constexpr void character(char c) {
        if (length < limit) {
            data[length++] = c;
        }
    }
    constexpr void text(std::string_view s) {
        for (const char byte : s) {
            const auto c = static_cast<unsigned char>(byte);
            character(c < 0x20 || (c >= 0x7f && c < 0xa0) ? '?' : static_cast<char>(c));
        }
    }
    constexpr void spaces(unsigned count) {
        while (count--) {
            character(' ');
        }
    }
    constexpr void field(std::string_view s, unsigned width) {
        unsigned n = 0;
        for (const char byte : s) {
            const auto c = static_cast<unsigned char>(byte);
            if (n == width) {
                break;
            }
            character(c < 0x20 || (c >= 0x7f && c < 0xa0) ? '?' : static_cast<char>(c));
            ++n;
        }
        spaces(width - n);
    }
    constexpr void right_field(std::string_view s, unsigned width) {
        if (width > s.size()) {
            spaces(width - s.size());
        }
        field(s, std::min<unsigned>(width, s.size()));
    }
    constexpr std::span<const char> view() const { return {data.data(), length}; }
    constexpr void number(ULONG value, unsigned width, bool negative) {
        std::array<char, 10> digits;
        unsigned n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        const unsigned used = n + (negative ? 1 : 0);
        if (width > used) {
            spaces(width - used);
        }
        if (negative) {
            character('-');
        }
        while (n) {
            character(digits[--n]);
        }
    }
    constexpr void decimal(ULONG value, unsigned width = 0) { number(value, width, false); }
    constexpr void signed_decimal(LONG value, unsigned width = 0) {
        const bool negative = value < 0;
        const ULONG magnitude =
            negative ? 0UL - static_cast<ULONG>(value) : static_cast<ULONG>(value);
        number(magnitude, width, negative);
    }
    constexpr void hex8(ULONG value) {
        static constexpr char digits[] = "0123456789ABCDEF";
        for (int shift = 28; shift >= 0; shift -= 4) {
            character(digits[(value >> shift) & 15]);
        }
    }
};

struct Screen {
    std::array<std::array<char, max_cols>, max_rows> previous = {};
    std::array<unsigned, max_rows> previous_length = {};
    std::array<bool, max_rows> selected = {};
    std::array<bool, max_rows> valid = {};
    unsigned rows = 0;
    unsigned cols = 0;

    void size(unsigned height, unsigned width) {
        if (height == rows && width == cols) {
            return;
        }
        rows = height;
        cols = width;
        valid.fill(false);
        PutStr("\2330m\233 p\233H\233J");
    }

    void line(unsigned row, const Line &content, bool highlight = false) {
        if (row == 0 || row > rows) {
            return;
        }
        const unsigned index = row - 1;
        const bool same =
            valid[index] && selected[index] == highlight &&
            previous_length[index] == content.length &&
            std::ranges::equal(content.view(), std::span{previous[index].data(), content.length});
        if (same) {
            return;
        }
        Printf("\233%lu;1H", static_cast<ULONG>(row));
        if (highlight) {
            PutStr("\2337m");
        }
        const auto text = content.view();
        shelltools::write_all(Output(), text.data(), static_cast<LONG>(text.size()));
        if (content.length < cols) {
            PutStr("\233K");
        }
        if (highlight) {
            PutStr("\2330m");
        }
        std::ranges::copy(content.view(), previous[index].begin());
        previous_length[index] = content.length;
        selected[index] = highlight;
        valid[index] = true;
    }
};

constexpr bool before(const TaskRow &a, const TaskRow &b, unsigned a_cpu, unsigned b_cpu,
                      char sort) {
    if (sort == 't' && a_cpu != b_cpu) {
        return a_cpu > b_cpu;
    }
    if (sort == 'p' && a.priority != b.priority) {
        return a.priority > b.priority;
    }
    if (sort == 's' && a.state != b.state) {
        return a.state < b.state;
    }
    if (sort == 'c' && a.cli != b.cli) {
        return a.cli < b.cli;
    }
    const int names = compare_text(a.name.data(), b.name.data());
    return names ? names < 0 : a.address < b.address;
}

void prepare(View &view) {
    view.shown = 0;
    for (unsigned i = 0; i < view.snapshot.count; ++i) {
        const TaskRow &row = view.snapshot.rows[i];
        const std::string_view filter{view.filter.data()};
        if (!contains(row.name.data(), filter) && !contains(row.command.data(), filter)) {
            continue;
        }
        unsigned at = view.shown++;
        while (at && before(row, view.snapshot.rows[view.order[at - 1]], view.cpu.tenths[i],
                            view.cpu.tenths[view.order[at - 1]], view.sort)) {
            view.order[at] = view.order[at - 1];
            --at;
        }
        view.order[at] = i;
    }
    if (view.selected >= view.shown) {
        view.selected = view.shown ? view.shown - 1 : 0;
    }
    const unsigned page = view.rows >= 14 ? view.rows - 10 : 1;
    if (view.selected < view.scroll) {
        view.scroll = view.selected;
    }
    if (view.selected >= view.scroll + page) {
        view.scroll = view.selected - page + 1;
    }
}

constexpr void memory_bar(Line &line, std::string_view label, ULONG free, ULONG total) {
    line.field(label, 4);
    line.character(' ');
    line.character('\253'); // ISO-8859-1 left guillemet
    const ULONG used = total > free ? total - free : 0;
    const unsigned width = line.limit >= 55 ? 20 : 10;
    const ULONG step = total / width + (total % width != 0);
    unsigned filled = step ? used / step : 0;
    if (filled > width) {
        filled = width;
    }
    for (unsigned i = 0; i < width; ++i) {
        line.character(i < filled ? '#' : '\267');
    }
    line.character('\273'); // ISO-8859-1 right guillemet
    line.character(' ');
    line.decimal(used / 1024);
    line.text("K / ");
    line.decimal(total / 1024);
    line.character('K');
}

constexpr void cpu_field(Line &line, unsigned tenths, unsigned samples) {
    if (!samples) {
        line.text("  --.-");
        return;
    }
    line.decimal(tenths / 10, 4);
    line.character('.');
    line.decimal(tenths % 10);
}

constexpr bool same_name(const char *live, std::string_view saved) {
    return live && std::string_view{live} == saved;
}

bool signal_matching(const List &list, const TaskRow &row) {
    for (const Node *node = list.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        Task *task = reinterpret_cast<Task *>(const_cast<Node *>(node));
        if (reinterpret_cast<ULONG>(task) == row.address && task->tc_Node.ln_Type == row.type &&
            same_name(task->tc_Node.ln_Name, row.name.data())) {
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
        same_name(current->tc_Node.ln_Name, row.name.data())) {
        Signal(current, SIGBREAKF_CTRL_C);
        sent = true;
    } else {
        sent = signal_matching(SysBase->TaskReady, row) || signal_matching(SysBase->TaskWait, row);
    }
    Permit();
    return sent;
}

void render(View &view, Screen &screen, LONG number) {
    screen.size(view.rows, view.cols);
    if (view.rows < 14 || view.cols < 40) {
        Line line(view.cols);
        line.text("WatchTasks: resize window (40x14 minimum)");
        screen.line(1, line);
        Flush(Output());
        return;
    }
    prepare(view);
    unsigned running = 0, ready = 0, waiting = 0;
    for (const TaskRow &row : std::span{view.snapshot.rows}.first(view.snapshot.count)) {
        const UBYTE state = row.state;
        if (state == TS_RUN) {
            ++running;
        } else if (state == TS_READY) {
            ++ready;
        } else if (state == TS_WAIT) {
            ++waiting;
        }
    }
    Line title(view.cols, true);
    title.text("WatchTasks  #");
    title.decimal(static_cast<ULONG>(number));
    title.text("  tasks ");
    title.decimal(view.snapshot.count);
    title.text("  sort ");
    title.character(view.sort);
    if (view.paused) {
        title.text("  PAUSED");
    }
    if (view.snapshot.truncated) {
        title.text("  TRUNCATED");
    }
    title.spaces(title.limit - title.length);
    screen.line(1, title, true);
    Line chip(view.cols);
    memory_bar(chip, "CHIP", view.chip_free, view.chip_total);
    screen.line(2, chip);
    Line fast(view.cols);
    memory_bar(fast, "FAST", view.fast_free, view.fast_total);
    screen.line(3, fast);
    Line status(view.cols);
    status.text("RUN ");
    status.decimal(running);
    status.text("  READY ");
    status.decimal(ready);
    status.text("  WAIT ");
    status.decimal(waiting);
    status.text("  CPU~ ");
    if (view.cpu.samples) {
        status.decimal(view.cpu.busy_tenths / 10);
        status.character('.');
        status.decimal(view.cpu.busy_tenths % 10);
        status.character('%');
    } else {
        status.text("--.-%");
    }
    status.text("  SAMPLES ");
    status.decimal(view.cpu.samples);
    screen.line(4, status);
    Line rule(view.cols);
    for (unsigned i = 0; i < rule.limit; ++i) {
        rule.character('-');
    }
    screen.line(5, rule);
    Line headings(view.cols);
    const bool compact = view.cols < 60;
    headings.spaces(2);
    headings.text("T/P ");
    if (!compact) {
        headings.field("TASK_ID", 8);
        headings.character(' ');
        headings.right_field("CLI", 5);
        headings.character(' ');
        headings.right_field("PRI", 4);
        headings.character(' ');
    }
    headings.right_field("CPU%", 6);
    headings.character(' ');
    if (!compact) {
        headings.field("STATE", 5);
        headings.character(' ');
    }
    headings.text("NAME");
    screen.line(6, headings);

    const unsigned page = view.rows - 10;
    if (view.help) {
        static constexpr std::array help = {
            "T/P  Exec task (thread) / DOS process",
            "j/k or arrows  select; n/u page down/up",
            "t/p/s/c/a  sort CPU/priority/state/CLI/name",
            "/  filter name or command; Enter accepts",
            "b  send Ctrl-C to selected task",
            "Space  pause/resume snapshots",
            "CPU%  VBlank sample estimate; -- means few samples",
            "h  close help; q  quit",
        };
        for (unsigned i = 0; i < page; ++i) {
            Line line(view.cols);
            if (i < help.size()) {
                line.text(help[i]);
            }
            screen.line(7 + i, line);
        }
    } else {
        for (unsigned i = 0; i < page; ++i) {
            const unsigned pos = view.scroll + i;
            Line line(view.cols, true);
            if (pos >= view.shown) {
                line.spaces(line.limit);
                screen.line(7 + i, line);
                continue;
            }
            const unsigned task_index = view.order[pos];
            const TaskRow &row = view.snapshot.rows[task_index];
            line.character(pos == view.selected ? '>' : ' ');
            line.character(' ');
            line.character(' ');
            line.character(row.type == NT_PROCESS ? 'P' : 'T');
            line.spaces(2);
            if (!compact) {
                line.hex8(row.address);
                line.character(' ');
                line.decimal(row.cli > 0 ? static_cast<ULONG>(row.cli) : 0, 5);
                line.character(' ');
                line.signed_decimal(row.priority, 4);
                line.character(' ');
            }
            cpu_field(line, view.cpu.tenths[task_index], view.cpu.samples);
            line.character(' ');
            if (!compact) {
                line.field(shelltools::task_state(row.state), 5);
                line.character(' ');
            }
            line.text(row.name.data());
            line.spaces(line.limit - line.length);
            screen.line(7 + i, line, pos == view.selected);
        }
    }
    Line detail(view.cols);
    Line signals(view.cols);
    if (view.shown) {
        const TaskRow &row = view.snapshot.rows[view.order[view.selected]];
        detail.text("SELECT ");
        detail.decimal(view.selected + 1);
        detail.character('/');
        detail.decimal(view.shown);
        detail.text("  ");
        detail.text(row.type == NT_PROCESS ? "PROCESS" : "TASK");
        detail.text("  TASK ");
        detail.hex8(row.address);
        detail.text("  PRI ");
        detail.signed_decimal(row.priority);
        detail.text("  CLI ");
        detail.decimal(row.cli > 0 ? static_cast<ULONG>(row.cli) : 0);
        if (row.command.front() != '\0') {
            detail.text("  CMD ");
            detail.text(row.command.data());
        }
        signals.text("SIGNALS ");
        signals.hex8(row.signal_alloc);
        signals.character('/');
        signals.hex8(row.signal_wait);
        signals.character('/');
        signals.hex8(row.signal_received);
        signals.text("  STACK ");
        signals.hex8(row.stack_lower);
        signals.character('-');
        signals.hex8(row.stack_upper);
    } else {
        detail.text("No matching tasks");
    }
    screen.line(view.rows - 3, detail);
    screen.line(view.rows - 2, signals);
    Line filter(view.cols);
    filter.text("Filter: ");
    filter.text(view.filter.data());
    if (view.editing) {
        filter.character('_');
    } else {
        filter.text("  ");
        filter.text(view.message);
    }
    screen.line(view.rows - 1, filter);
    Line shortcuts(view.cols);
    shortcuts.text(
        view.cols < 80
            ? "j/k  t CPU sort  / filter  h help  q quit"
            : "j/k move  n/u page  t/p/s/c/a sort  / filter  b break  h help  Space pause q quit");
    screen.line(view.rows, shortcuts);
    Flush(Output());
}

struct RawConsole {
    BPTR input;
    bool active;
    unsigned rows = 24;
    unsigned cols = 80;
    bool size_known = false;
    bool size_changed = false;
    std::array<char, 40> sequence = {};
    unsigned sequence_length = 0;
    unsigned escape = 0;
    std::array<char, 16> keys = {};
    unsigned head = 0;
    unsigned tail = 0;

    explicit RawConsole(BPTR in) : input(in), active(SetMode(in, 1) != 0) {}
    ~RawConsole() {
        if (active) {
            PutStr("\2330m\2330 p\233H\233J");
            Flush(Output());
            SetMode(input, 0);
        }
    }

    void enqueue(char value) {
        const unsigned next = (tail + 1) % keys.size();
        if (next != head) {
            keys[tail] = value;
            tail = next;
        }
    }

    int key() {
        if (head == tail) {
            return -1;
        }
        const UBYTE value = static_cast<UBYTE>(keys[head]);
        head = (head + 1) % keys.size();
        return value;
    }

    void report_size() {
        // Amiga console aWBR: CSI 1;1;<rows>;<cols> SP r.
        if (sequence_length < 9 || sequence[sequence_length - 2] != ' ' ||
            sequence[sequence_length - 1] != 'r') {
            return;
        }
        std::array<ULONG, 4> value = {};
        unsigned part = 0;
        bool digit = false;
        for (unsigned i = 0; i + 2 < sequence_length; ++i) {
            const char c = sequence[i];
            if (c >= '0' && c <= '9') {
                if (value[part] > 999) {
                    return;
                }
                value[part] = value[part] * 10 + static_cast<ULONG>(c - '0');
                digit = true;
            } else if (c == ';' && digit && part < 3) {
                ++part;
                digit = false;
            } else {
                return;
            }
        }
        if (!digit || part != 3 || value[0] != 1 || value[1] != 1 || value[2] == 0 ||
            value[3] == 0) {
            return;
        }
        const unsigned height = value[2] > max_rows ? max_rows : value[2];
        const unsigned width = value[3] > max_cols ? max_cols : value[3];
        if (!size_known || height != rows || width != cols) {
            size_changed = true;
        }
        rows = height;
        cols = width;
        size_known = true;
    }

    void accept(UBYTE byte) {
        if (escape == 0) {
            if (byte == 0x9b) {
                escape = 2;
                sequence_length = 0;
            } else if (byte == 27) {
                escape = 1;
            } else {
                enqueue(static_cast<char>(byte));
            }
            return;
        }
        if (escape == 1) {
            escape = 0;
            if (byte == '[') {
                escape = 2;
                sequence_length = 0;
            } else {
                enqueue(27);
                accept(byte);
            }
            return;
        }
        if (sequence_length + 1 >= sequence.size()) {
            escape = 0;
            return;
        }
        sequence[sequence_length++] = static_cast<char>(byte);
        if (byte < 0x40 || byte > 0x7e) {
            return;
        }
        escape = 0;
        if (sequence_length == 1 && byte == 'A') {
            enqueue('k');
        } else if (sequence_length == 1 && byte == 'B') {
            enqueue('j');
        } else if (byte == 'r') {
            report_size();
        }
    }

    bool poll(LONG micros) {
        if (!WaitForChar(input, micros)) {
            if (escape == 1) {
                escape = 0;
                enqueue(27);
            }
            return false;
        }
        UBYTE byte = 0;
        if (Read(input, &byte, 1) == 1) {
            accept(byte);
            return true;
        }
        return false;
    }

    static void request_size() {
        PutStr("\2330 q");
        Flush(Output());
    }

    void detect_size() {
        request_size();
        unsigned idle = 0;
        for (unsigned i = 0; i < 64 && idle < 10 && !size_known; ++i) {
            idle = poll(20000) ? 0 : idle + 1;
        }
    }
};

int watch_snapshots(LONG ticks, LONG count, bool machine);

void handle_key(View &view, char key, bool &done) {
    if (view.editing) {
        const auto len = std::string_view{view.filter.data()}.size();
        if (key == '\r' || key == '\n' || key == 27) {
            view.editing = false;
        } else if ((key == 8 || key == 127) && len) {
            view.filter[len - 1] = '\0';
        } else if (static_cast<UBYTE>(key) >= 32 && static_cast<UBYTE>(key) < 127 &&
                   len + 1 < view.filter.size()) {
            view.filter[len] = key;
            view.filter[len + 1] = '\0';
        }
        view.selected = 0;
        view.scroll = 0;
        return;
    }
    if (key == 'q' || key == 'Q') {
        done = true;
    } else if (key == 'h' || key == 'H' || key == '?') {
        view.help = !view.help;
    } else if (key == ' ') {
        view.paused = !view.paused;
    } else if (key == '/') {
        view.editing = true;
    } else if (key == 'j') {
        if (view.selected + 1 < view.shown) {
            ++view.selected;
        }
    } else if (key == 'k') {
        if (view.selected) {
            --view.selected;
        }
    } else if (key == 'n') {
        view.selected += view.rows >= 14 ? view.rows - 10 : 1;
    } else if (key == 'u') {
        view.selected = view.selected > (view.rows >= 14 ? view.rows - 10 : 1)
                            ? view.selected - (view.rows >= 14 ? view.rows - 10 : 1)
                            : 0;
    } else if (key == 't' || key == 'p' || key == 's' || key == 'c' || key == 'a') {
        view.sort = key;
        view.selected = 0;
        view.scroll = 0;
    } else if (key == 'b') {
        const bool sent =
            view.shown && break_selected(view.snapshot.rows[view.order[view.selected]]);
        view.message = sent ? "Ctrl-C sent" : "Task changed or missing";
    }
}

int watch_interactive(LONG ticks, LONG count, unsigned requested_rows, unsigned requested_cols) {
    RawConsole console(Input());
    if (!console.active) {
        return watch_snapshots(ticks, count, false);
    }
    CpuSampler sampler;
    console.detect_size();
    static View view;
    static Screen screen;
    view.rows = requested_rows ? requested_rows : console.rows;
    view.cols = requested_cols ? requested_cols : console.cols;
    LONG number = 0;
    bool done = false;
    while (!done && (count == 0 || number < count)) {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            return shelltools::warn;
        }
        if (console.size_known) {
            console.request_size();
        }
        if (!view.paused || number == 0) {
            shelltools::capture_tasks(view.snapshot);
            view.cpu.update(sampler, view.snapshot);
            view.chip_free = AvailMem(MEMF_CHIP);
            view.chip_total = AvailMem(MEMF_CHIP | MEMF_TOTAL);
            view.fast_free = AvailMem(MEMF_FAST);
            view.fast_total = AvailMem(MEMF_FAST | MEMF_TOTAL);
            view.truncated |= view.snapshot.truncated;
            ++number;
        }
        render(view, screen, number);
        for (LONG step = 0; step < ticks && !done; ++step) {
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
                return shelltools::warn;
            }
            if (console.head == console.tail) {
                console.poll(20000);
            }
            if (console.size_changed) {
                console.size_changed = false;
                view.rows = requested_rows ? requested_rows : console.rows;
                view.cols = requested_cols ? requested_cols : console.cols;
                render(view, screen, number);
            }
            int key;
            while ((key = console.key()) >= 0 && !done) {
                if (key == 27 && view.editing) {
                    view.editing = false;
                } else {
                    handle_key(view, static_cast<char>(key), done);
                }
                if (!done) {
                    render(view, screen, number);
                }
            }
        }
    }
    return view.truncated ? shelltools::warn : shelltools::ok;
}

int watch_snapshots(LONG ticks, LONG count, bool machine) {
    static TaskSnapshot snapshot;
    static CpuUsage cpu;
    CpuSampler sampler;
    for (LONG round = 0; count == 0 || round < count; ++round) {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            return shelltools::warn;
        }
        Delay(ticks);
        shelltools::capture_tasks(snapshot);
        cpu.update(sampler, snapshot);
        if (machine) {
            Printf("{\"snapshot\":%ld,\"chip_free\":%lu,\"fast_free\":%lu}\n", round + 1,
                   AvailMem(MEMF_CHIP), AvailMem(MEMF_FAST));
        } else {
            Printf("SNAPSHOT=%ld CHIP_FREE=%lu FAST_FREE=%lu\n", round + 1, AvailMem(MEMF_CHIP),
                   AvailMem(MEMF_FAST));
            shelltools::print_task_header();
        }
        for (unsigned i = 0; i < snapshot.count; ++i) {
            shelltools::print_task(snapshot.rows[i], machine, round + 1,
                                  cpu.samples ? static_cast<LONG>(cpu.tenths[i]) : -1);
        }
        if (snapshot.truncated) {
            PutStr(machine ? "{\"truncated\":true}\n" : "TRUNCATED=1\n");
            return shelltools::warn;
        }
    }
    return shelltools::ok;
}

[[gnu::noinline, gnu::noipa]] int watch_main() {
    LONG args[6] = {};
    RDArgs *rd = ReadArgs("TICKS/N/K,COUNT/N/K,ROWS/N/K,COLS/N/K,MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("WatchTasks");
    }
    if (args[5]) {
        shelltools::help(
            "PURPOSE: Live task CPU and memory monitor\n"
            "USAGE: WatchTasks [TICKS=<n>] [COUNT=<n>] [ROWS=<n>] [COLS=<n>] [MACHINE]\n"
            "DEFAULT: 50 ticks, unlimited, auto console size\n"
            "T/P: T=Exec task (thread), P=DOS process\n"
            "KEYS: j/k move; t/p/s/c/a sort; / filter; b break; h help; q quit\n"
            "CPU: VBlank sample estimate; -- means few samples\n"
            "OUTPUT: Interactive ISO-8859-1 UI; MACHINE gives JSON lines\n"
            "RC: 0=done 5=break/truncated 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const LONG ticks = args[0] ? *reinterpret_cast<LONG *>(args[0]) : 50;
    const LONG count = args[1] ? *reinterpret_cast<LONG *>(args[1]) : 0;
    const LONG rows = args[2] ? *reinterpret_cast<LONG *>(args[2]) : 0;
    const LONG cols = args[3] ? *reinterpret_cast<LONG *>(args[3]) : 0;
    const bool machine = args[4] != 0;
    if (ticks <= 0 || count < 0 || (rows && (rows < 14 || rows > 100)) ||
        (cols && (cols < 40 || cols > 160))) {
        PutStr("WatchTasks: TICKS>0 COUNT>=0 ROWS=14..100 COLS=40..160\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    FreeArgs(rd);
    if (!machine && IsInteractive(Input()) && IsInteractive(Output())) {
        return watch_interactive(ticks, count, static_cast<unsigned>(rows),
                                 static_cast<unsigned>(cols));
    }
    return watch_snapshots(ticks, count, machine);
}

constexpr ULONG private_stack_size = 16UL * 1024UL;
StackSwapStruct private_stack_swap = {};
int private_stack_result = shelltools::error;

// Keep this frame free of locals: it spans both StackSwap calls.
[[gnu::noinline, gnu::noipa]] void watch_trampoline() {
    StackSwap(&private_stack_swap);
    private_stack_result = watch_main();
    StackSwap(&private_stack_swap);
}

} // namespace

int main() {
    APTR stack = AllocMem(private_stack_size, MEMF_ANY);
    if (!stack) {
        PutStr("WatchTasks: no memory for private stack\n");
        return shelltools::error;
    }
    UBYTE *upper = static_cast<UBYTE *>(stack) + private_stack_size;
    private_stack_swap.stk_Lower = stack;
    private_stack_swap.stk_Upper = reinterpret_cast<ULONG>(upper);
    private_stack_swap.stk_Pointer = upper;
    watch_trampoline();
    FreeMem(stack, private_stack_size);
    return private_stack_result;
}
