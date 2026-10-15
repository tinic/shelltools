#include "task_output.hpp"
#include "task_snapshot.hpp"
#include "tool_support.hpp"

#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <span>
#include <string_view>

namespace {

using shelltools::copy_command;
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

struct TaskDetails {
    TaskRow row = {};
    unsigned scroll = 0;
    unsigned lines = 0;
    unsigned cpu_tenths = 0;
    unsigned samples = 0;
    bool active = false;
    bool present = false;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
struct View {
    // Static storage zero-initializes these large buffers without file data.
    // cppcheck-suppress uninitMemberVarNoCtor
    TaskSnapshot snapshot;
    CpuUsage cpu;
    TaskDetails details;
    TaskRow selected_task = {};
    TaskRow break_target = {};
    // cppcheck-suppress uninitMemberVarNoCtor
    std::array<unsigned, TaskSnapshot::capacity> order;
    unsigned shown = 0;
    unsigned selected = 0;
    unsigned scroll = 0;
    unsigned help_scroll = 0;
    unsigned rows = 24;
    unsigned cols = 80;
    std::array<char, 32> filter = {};
    const char *message = "";
    char sort = 't';
    bool paused = false;
    bool help = false;
    bool editing = false;
    bool selection_valid = false;
    bool confirming_break = false;
    bool break_target_missing = false;
    bool truncated = false;
    ULONG chip_free = 0;
    ULONG chip_total = 0;
    ULONG fast_free = 0;
    ULONG fast_total = 0;
};
#pragma GCC diagnostic pop

constexpr unsigned max_rows = 100;
constexpr unsigned max_cols = 160;
constexpr unsigned task_page_rows(unsigned rows) {
    return rows >= 14 ? rows - 9 : 1;
}
constexpr unsigned help_page_rows(unsigned rows) {
    return rows >= 14 ? rows - 3 : 1;
}
constexpr unsigned details_height(unsigned rows) {
    return rows >= 14 ? std::min(rows - 4, 28U) : 0;
}
constexpr unsigned details_page_rows(unsigned rows) {
    return rows >= 14 ? details_height(rows) - 2 : 1;
}
// Two help columns, the cpu bar column and the wide key bar from here up.
constexpr unsigned wide_cols = 76;

// Console renditions, each a whole SGR so none inherits another's pens.  Pens
// 0-3 only, which every Workbench screen has: 0 grey, 1 black, 2 white, 3 blue
// on the default palette.  Position and text carry the meaning; colour only
// groups it.
enum class Attr : UBYTE {
    normal,
    bold,
    bar,      // title and key bars: white on blue
    bar_key,  // a key on a bar
    bar_dim,  // what the key does: black on blue
    head,     // column headings, help sections: black on white
    head_on,  // the sort column
    select,   // the selected task: white on black
    fill,     // a bar's filled cells: blue
    fill_dark,
    alert,    // PAUSED, messages, a missing task: reversed
    count
};

constexpr std::array<const char *, static_cast<unsigned>(Attr::count)> renditions = {
    "\2330m",          "\2330;1m",       "\2330;32;43m",  "\2330;1;32;43m",
    "\2330;31;43m",    "\2330;31;42m",   "\2330;1;32;41m", "\2330;1;32;41m",
    "\2330;43m",       "\2330;41m",      "\2330;1;7m"};

struct HelpItem {
    std::string_view key;
    std::string_view text;
    bool section = false;
};

constexpr HelpItem section(std::string_view title) {
    return HelpItem{title, "", true};
}

constexpr std::array<std::array<HelpItem, 2>, 16> wide_help = {{
    {section("MOVE / SELECT"), section("SORT")},
    {HelpItem{"j/k Up/Down", "One row"}, HelpItem{"t", "CPU"}},
    {HelpItem{"n/u", "One page"}, HelpItem{"p", "Priority"}},
    {HelpItem{"Enter", "Task details"}, HelpItem{"s", "State"}},
    {HelpItem{"Esc", "Close details"}, HelpItem{"c", "CLI"}},
    {HelpItem{}, HelpItem{"a", "Name"}},
    {section("FILTER"), HelpItem{}},
    {HelpItem{"/", "Filter name/command"}, section("ACTIONS")},
    {HelpItem{"Backspace", "Delete"}, HelpItem{"Space", "Pause / resume"}},
    {HelpItem{"Enter/Esc", "Finish filter"}, HelpItem{"b", "Confirm Ctrl-C"}},
    {HelpItem{}, HelpItem{"h", "Close help"}},
    {section("TABLE"), HelpItem{"q", "Quit"}},
    {HelpItem{"T", "Exec task"}, HelpItem{}},
    {HelpItem{"P", "DOS process"}, section("CPU")},
    {HelpItem{"TASK_ID", "Exec address"}, HelpItem{"CPU%", "VBlank sample estimate"}},
    {HelpItem{}, HelpItem{"--", "Fewer than 10 samples"}},
}};

constexpr std::array narrow_help = {
    section("MOVE / SELECT"),
    HelpItem{"j/k, arrows", "One row"},
    HelpItem{"n/u", "One page"},
    HelpItem{"Enter", "Task details"},
    HelpItem{"Esc", "Close details"},
    section("SORT"),
    HelpItem{"t  p", "CPU  Priority"},
    HelpItem{"s  c  a", "State  CLI  Name"},
    section("FILTER"),
    HelpItem{"/", "Filter name/command"},
    HelpItem{"Backspace", "Delete"},
    HelpItem{"Enter/Esc", "Finish filter"},
    section("ACTIONS"),
    HelpItem{"Space", "Pause / resume"},
    HelpItem{"b", "Confirm Ctrl-C"},
    HelpItem{"h  q", "Close help  Quit"},
    section("TABLE / CPU"),
    HelpItem{"T  P", "Exec task  DOS process"},
    HelpItem{"CPU%", "VBlank sample estimate"},
    HelpItem{"--", "Fewer than 10 samples"},
};

constexpr unsigned help_count(unsigned cols) {
    return static_cast<unsigned>(cols >= wide_cols ? wide_help.size() : narrow_help.size());
}

struct Line {
    std::array<char, max_cols> data = {};
    std::array<Attr, max_cols> attr = {};
    unsigned length = 0;
    unsigned limit = 0;
    Attr pen = Attr::normal;

    constexpr Line() = default;
    constexpr explicit Line(unsigned cols, bool full_width = false)
        : limit(full_width ? std::min(cols, max_cols)
                           : (cols > 1 ? std::min(cols - 1, max_cols - 1) : 0)) {}

    constexpr Line &use(Attr a) {
        pen = a;
        return *this;
    }
    constexpr void character(char c) {
        if (length < limit) {
            attr[length] = pen;
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
    constexpr void fill() { spaces(limit - length); }
    constexpr unsigned room() const { return limit - length; }
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
            spaces(static_cast<unsigned>(width - s.size()));
        }
        field(s, std::min<unsigned>(width, static_cast<unsigned>(s.size())));
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
    // A key and what it does, on a bar; nothing when it does not fit whole.
    constexpr bool key(std::string_view name, std::string_view action) {
        if (name.size() + action.size() + 3 > room()) {
            return false;
        }
        use(Attr::bar_key).text(name);
        use(Attr::bar_dim).character(' ');
        text(action);
        spaces(2);
        return true;
    }
    // `label` then `value` in bold, or nothing when both do not fit whole.
    constexpr void pair(std::string_view label, std::string_view value) {
        if (label.size() + value.size() > room()) {
            limit = length;
            return;
        }
        use(Attr::normal).text(label);
        use(Attr::bold).text(value);
        use(Attr::normal);
    }
};

// `used` of `total` as `width` cells: filled cells, then middle dots.
constexpr void bar(Line &line, ULONG used, ULONG total, unsigned width, Attr filled) {
    unsigned cells = 0;
    if (total && used) {
        while (total > 0xffffUL) {
            total >>= 1;
            used >>= 1;
        }
        cells = static_cast<unsigned>(std::min<ULONG>(width, (used * width + total - 1) / total));
    }
    line.use(filled).spaces(cells);
    line.use(Attr::normal);
    while (cells++ < width) {
        line.character('\267'); // ISO-8859-1 middle dot
    }
}

struct Panel {
    std::array<Line, 28> rows = {};
    unsigned top = 0;
    unsigned left = 0;
    unsigned width = 0;
    unsigned height = 0;

    void size(unsigned screen_rows, unsigned screen_cols) {
        height = details_height(screen_rows);
        width = std::min(screen_cols - 4, 104U);
        top = (screen_rows - height) / 2 + 1;
        left = (screen_cols - width) / 2;
        for (Line &row : std::span{rows}.first(height)) {
            row = Line(width, true);
            row.use(Attr::fill).character(' ');
            row.use(Attr::normal).spaces(width - 2);
            row.use(Attr::fill).character(' ');
        }
    }

    // Inside the edges with a space each side.
    constexpr unsigned inner() const { return width - 4; }

    void content(unsigned index, const Line &line) {
        Line &row = rows[index];
        const unsigned n = std::min(line.length, inner());
        std::ranges::copy(std::span{line.data}.first(n), row.data.begin() + 2);
        std::ranges::copy(std::span{line.attr}.first(n), row.attr.begin() + 2);
    }

    void set(unsigned index, const Line &line) { rows[index] = line; }
};

// A frame's output, written with one Write() so console.device never sees a
// control sequence split between two writes and the window does not flicker.
struct Frame {
    std::array<char, 4096> data = {};
    unsigned used = 0;
    bool lost = false; // a write failed: the screen no longer shows the cache

    void flush() {
        if (!used) {
            return;
        }
        Flush(Output());
        const char *at = data.data();
        LONG left = static_cast<LONG>(used);
        while (left > 0) {
            const LONG wrote = Write(Output(), at, left);
            if (wrote <= 0) {
                lost = true;
                break;
            }
            at += wrote;
            left -= wrote;
        }
        used = 0;
    }
    void bytes(std::span<const char> text) {
        if (used + text.size() > data.size()) {
            flush();
        }
        for (const char c : text.first(std::min<std::size_t>(text.size(), data.size()))) {
            data[used++] = c;
        }
    }
    void put(std::string_view text) { bytes(std::span<const char>{text.data(), text.size()}); }
    void row(unsigned number) {
        std::array<char, 8> s = {'\233'};
        unsigned n = 1;
        if (number >= 100) {
            s[n++] = static_cast<char>('0' + number / 100);
        }
        if (number >= 10) {
            s[n++] = static_cast<char>('0' + number / 10 % 10);
        }
        s[n++] = static_cast<char>('0' + number % 10);
        s[n++] = ';';
        s[n++] = '1';
        s[n++] = 'H';
        bytes(std::span<const char>{s.data(), n});
    }
};

Frame frame;

const Line blank;

struct Screen {
    std::array<std::array<char, max_cols>, max_rows> previous = {};
    std::array<std::array<Attr, max_cols>, max_rows> previous_attr = {};
    std::array<unsigned, max_rows> previous_length = {};
    std::array<bool, max_rows> valid = {};
    const Panel *panel = nullptr;
    unsigned rows = 0;
    unsigned cols = 0;

    void size(unsigned height, unsigned width) {
        if (frame.lost) {
            // Part of an earlier frame never arrived: redraw every row.
            frame.lost = false;
            valid.fill(false);
        }
        if (height == rows && width == cols) {
            return;
        }
        rows = height;
        cols = width;
        valid.fill(false);
        // Cursor off, no wrap or scroll, then a form feed: console.device
        // resets the bitplanes it renders and clears through only on a full
        // clear, so a CSI J alone can leave a coloured cell's other planes.
        frame.put("\2330m\2330 p\233?7l\233>1l\f");
    }

    static void emit(const Line &text) {
        Attr current = Attr::normal;
        unsigned start = 0;
        while (start < text.length) {
            unsigned end = start + 1;
            while (end < text.length && text.attr[end] == text.attr[start]) {
                ++end;
            }
            if (text.attr[start] != current) {
                current = text.attr[start];
                frame.put(renditions[static_cast<unsigned>(current)]);
            }
            frame.bytes(std::span<const char>{text.data.data() + start, end - start});
            start = end;
        }
        if (current != Attr::normal) {
            frame.put(renditions[0]);
        }
    }

    void line(unsigned row, const Line &content) {
        if (row == 0 || row > rows) {
            return;
        }
        const bool beside = panel && row > 1 && row < rows;
        const Line *cover = beside ? (row >= panel->top && row < panel->top + panel->height
                                          ? &panel->rows[row - panel->top]
                                          : &blank)
                                   : nullptr;
        static Line composed;
        composed = content;
        if (cover) {
            // The table around the overlay is left blank.
            composed = Line(cols, true);
            composed.fill();
            const unsigned n = std::min(cover->length, composed.limit - panel->left);
            std::ranges::copy(std::span{cover->data}.first(n), composed.data.begin() + panel->left);
            std::ranges::copy(std::span{cover->attr}.first(n), composed.attr.begin() + panel->left);
        }
        const unsigned index = row - 1;
        const unsigned n = composed.length;
        if (valid[index] && previous_length[index] == n &&
            std::ranges::equal(composed.view(), std::span{previous[index]}.first(n)) &&
            std::ranges::equal(std::span{composed.attr}.first(n),
                               std::span{previous_attr[index]}.first(n))) {
            return;
        }
        frame.row(row);
        emit(composed);
        if (n < cols) {
            frame.put("\233K");
        }
        std::ranges::copy(composed.view(), previous[index].begin());
        std::ranges::copy(std::span{composed.attr}.first(n), previous_attr[index].begin());
        previous_length[index] = n;
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

constexpr bool same_task(const TaskRow &a, const TaskRow &b) {
    return a.address == b.address && a.type == b.type &&
           std::string_view{a.name.data()} == std::string_view{b.name.data()};
}

constexpr bool same_break_target(const TaskRow &a, const TaskRow &b) {
    return same_task(a, b) && a.cli == b.cli &&
           std::string_view{a.command.data()} == std::string_view{b.command.data()};
}

void prepare(View &view) {
    if (view.confirming_break && !view.break_target_missing) {
        const auto captured = std::span{view.snapshot.rows}.first(view.snapshot.count);
        view.break_target_missing = !std::ranges::any_of(captured, [&view](const TaskRow &row) {
            return same_break_target(row, view.break_target);
        });
    }
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
    if (view.selection_valid) {
        for (unsigned i = 0; i < view.shown; ++i) {
            if (same_task(view.snapshot.rows[view.order[i]], view.selected_task)) {
                view.selected = i;
                break;
            }
        }
    }
    if (view.selected >= view.shown) {
        view.selected = view.shown ? view.shown - 1 : 0;
    }
    view.selection_valid = view.shown != 0;
    if (view.selection_valid) {
        view.selected_task = view.snapshot.rows[view.order[view.selected]];
    }
    const unsigned page = task_page_rows(view.rows);
    if (view.selected < view.scroll) {
        view.scroll = view.selected;
    }
    if (view.selected >= view.scroll + page) {
        view.scroll = view.selected - page + 1;
    }
}

constexpr unsigned meter_width(unsigned cols) {
    return cols >= 72 ? 32 : (cols >= 52 ? cols - 40 : 12);
}

constexpr void memory_meter(Line &line, std::string_view label, ULONG free, ULONG total,
                            Attr filled) {
    const ULONG used = total > free ? total - free : 0;
    line.use(Attr::normal).text("  ");
    line.field(label, 6);
    bar(line, used, total, meter_width(line.limit + 1), filled);
    line.character(' ');
    line.use(Attr::bold);
    line.decimal(used / 1024, 6);
    line.character('K');
    line.use(Attr::normal).text(" of ");
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

void refresh_details(View &view) {
    TaskDetails &details = view.details;
    details.present = false;
    for (unsigned i = 0; i < view.snapshot.count; ++i) {
        if (same_task(view.snapshot.rows[i], details.row)) {
            details.row = view.snapshot.rows[i];
            details.cpu_tenths = view.cpu.tenths[i];
            details.samples = view.cpu.samples;
            details.present = true;
            return;
        }
    }
    details.samples = 0;
}

struct DetailWriter {
    Panel &panel;
    unsigned scroll;
    unsigned count = 0;

    DetailWriter(Panel &window, unsigned offset) : panel(window), scroll(offset) {}

    constexpr unsigned width() const { return panel.inner(); }

    void line(const Line &text) {
        if (count >= scroll && count - scroll < panel.height - 2) {
            panel.content(count - scroll + 1, text);
        }
        ++count;
    }

    Line labelled(std::string_view label) const {
        Line text(width(), true);
        text.field(label, 12);
        text.use(Attr::bold);
        return text;
    }

    void field(std::string_view label, std::string_view value, Attr tone = Attr::bold) {
        if (value.empty()) {
            value = "-";
        }
        while (!value.empty()) {
            Line text = labelled(label);
            text.use(tone);
            const auto part = std::min<std::size_t>(value.size(), width() - 12);
            text.text(std::string_view{value.data(), part});
            line(text);
            value.remove_prefix(part);
            label = "";
        }
    }

    void decimal(std::string_view label, ULONG value) {
        Line text = labelled(label);
        text.decimal(value);
        line(text);
    }

    void hex(std::string_view label, ULONG value) {
        Line text = labelled(label);
        text.hex8(value);
        line(text);
    }
};

unsigned fill_details(const View &view, Panel &panel) {
    const TaskDetails &details = view.details;
    const TaskRow &row = details.row;
    DetailWriter writer{panel, details.scroll};
    if (!details.present) {
        writer.field("STATUS", "MISSING: last captured values", Attr::alert);
    } else {
        writer.field("STATUS", view.paused ? "PAUSED" : "LIVE", view.paused ? Attr::alert
                                                                          : Attr::bold);
    }
    writer.field("NAME", row.name.data());
    writer.field("TYPE", row.type == NT_PROCESS ? "P: DOS process" : "T: Exec task");
    writer.hex("TASK ID", row.address);
    if (row.type == NT_PROCESS) {
        if (row.cli > 0) {
            writer.decimal("CLI", static_cast<ULONG>(row.cli));
        } else {
            writer.field("CLI", "None");
        }
        writer.field("COMMAND", row.command.data());
        std::string_view arguments{row.arguments.data()};
        while (arguments.ends_with('\n') || arguments.ends_with('\r')) {
            arguments.remove_suffix(1);
        }
        writer.field("ARGUMENTS", arguments);
    }
    Line priority = writer.labelled("PRIORITY");
    priority.signed_decimal(row.priority);
    priority.use(Attr::normal).text("  STATE ");
    priority.use(Attr::bold).text(shelltools::task_state(row.state));
    writer.line(priority);
    Line cpu = writer.labelled("CPU%");
    cpu_field(cpu, details.cpu_tenths, details.samples);
    if (details.samples && cpu.room() > 4) {
        cpu.character(' ');
        cpu.character(' ');
        bar(cpu, details.cpu_tenths, 1000, std::min(cpu.room(), 20U), Attr::fill);
    }
    writer.line(cpu);
    writer.decimal("STACK BYTES",
                   row.stack_upper >= row.stack_lower ? row.stack_upper - row.stack_lower : 0);
    Line bounds = writer.labelled("STACK RANGE");
    bounds.hex8(row.stack_lower);
    bounds.use(Attr::normal).character('-');
    bounds.use(Attr::bold).hex8(row.stack_upper);
    writer.line(bounds);
    writer.hex("SAVED SP", row.stack_pointer);
    writer.hex("SIG ALLOC", row.signal_alloc);
    writer.hex("SIG WAIT", row.signal_wait);
    writer.hex("SIG RECV", row.signal_received);
    writer.hex("TASK FLAGS", row.flags);
    if (row.type == NT_PROCESS) {
        Line stack = writer.labelled("PROC STACK");
        stack.signed_decimal(row.process_stack_size);
        writer.line(stack);
        writer.hex("PROC FLAGS", static_cast<ULONG>(row.process_flags));
    }
    writer.field("NOTE", "Saved SP may be stale", Attr::normal);
    return writer.count;
}

struct KeyItem {
    std::string_view key;
    std::string_view action;
    unsigned rank; // 0 always; higher ranks go first when the bar is short
};

// A key bar: as many keys as fit, the lowest ranks first, shown in order.
void key_bar(Line &line, std::span<const KeyItem> items) {
    line.use(Attr::bar).character(' ');
    unsigned room = line.room();
    unsigned shown = 0; // ranks up to this one fit
    for (unsigned rank = 0; rank < 4; ++rank) {
        const unsigned need = std::accumulate(
            items.begin(), items.end(), 0U, [rank](unsigned sum, const KeyItem &item) {
                return item.rank <= rank
                           ? sum + static_cast<unsigned>(item.key.size() + item.action.size() + 3)
                           : sum;
            });
        if (need > room) {
            break;
        }
        shown = rank + 1;
    }
    for (const KeyItem &item : items) {
        if (item.rank < shown) {
            line.key(item.key, item.action);
        }
    }
    line.use(Attr::bar).fill();
}

void render_details(View &view, Panel &panel) {
    refresh_details(view);
    panel.size(view.rows, view.cols);
    TaskDetails &details = view.details;
    details.lines = fill_details(view, panel);
    const unsigned page = details_page_rows(view.rows);
    const unsigned end = details.lines > page ? details.lines - page : 0;
    if (details.scroll > end) {
        details.scroll = end;
        panel.size(view.rows, view.cols);
        fill_details(view, panel);
    }
    Line range(panel.width, true);
    range.decimal(details.scroll + 1);
    range.character('-');
    range.decimal(std::min(details.lines, details.scroll + page));
    range.character('/');
    range.decimal(details.lines);
    range.character(' ');
    Line title(panel.width, true);
    title.use(Attr::bar_key).text(" TASK DETAILS ");
    title.use(Attr::bar).spaces(title.room() > range.length ? title.room() - range.length : 0);
    title.text(std::string_view{range.data.data(), range.length});
    panel.set(0, title);
    Line keys(panel.width, true);
    static constexpr std::array<KeyItem, 3> detail_keys = {
        {{"Enter/Esc", "close", 0}, {"j/k", "scroll", 1}, {"n/u", "page", 2}}};
    key_bar(keys, detail_keys);
    panel.set(panel.height - 1, keys);
}

constexpr bool same_name(const char *live, std::string_view saved) {
    return live && std::string_view{live} == saved;
}

// Called under Forbid(): the prompt's CLI number and command must still hold.
bool live_break_target(const Task *task, const TaskRow &row) {
    if (reinterpret_cast<ULONG>(task) != row.address || task->tc_Node.ln_Type != row.type ||
        !same_name(task->tc_Node.ln_Name, row.name.data())) {
        return false;
    }
    if (task->tc_Node.ln_Type != NT_PROCESS) {
        return true;
    }
    const auto *process = reinterpret_cast<const Process *>(task);
    decltype(row.command) command{};
    copy_command(command, process);
    return process->pr_TaskNum == row.cli &&
           std::string_view{command.data()} == std::string_view{row.command.data()};
}

bool signal_matching(const List &list, const TaskRow &row) {
    for (const Node *node = list.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        Task *task = reinterpret_cast<Task *>(const_cast<Node *>(node));
        if (live_break_target(task, row)) {
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
    if (live_break_target(current, row)) {
        Signal(current, SIGBREAKF_CTRL_C);
        sent = true;
    } else {
        sent = signal_matching(SysBase->TaskReady, row) || signal_matching(SysBase->TaskWait, row);
    }
    Permit();
    return sent;
}

void render_break_confirmation(View &view, Panel &panel) {
    panel.size(view.rows, view.cols);
    panel.height = 10;
    panel.top = (view.rows - panel.height) / 2 + 1;
    Line title(panel.width, true);
    title.use(Attr::bar_key).text(" ARE YOU SURE? ");
    title.use(Attr::bar).fill();
    panel.set(0, title);
    Line question(panel.inner(), true);
    question.text(view.break_target_missing ? "Task changed or missing"
                                            : "Send Ctrl-C to this task?");
    panel.content(2, question);
    Line name(panel.inner(), true);
    name.use(Attr::bold).text(view.break_target.name.data());
    panel.content(3, name);
    Line command(panel.inner(), true);
    if (view.break_target.command.front() != '\0') {
        command.text("Cmd ");
        command.text(view.break_target.command.data());
    }
    panel.content(4, command);
    Line identity(panel.inner(), true);
    identity.text(view.break_target.type == NT_PROCESS ? "Process ID " : "Exec task ID ");
    identity.hex8(view.break_target.address);
    panel.content(5, identity);
    Line warning(panel.inner(), true);
    warning.text("This can stop the selected task.");
    panel.content(7, warning);
    Line keys(panel.width, true);
    keys.use(Attr::bar).character(' ');
    if (!view.break_target_missing) {
        keys.key("Y", "send");
    }
    keys.key("Enter/Esc/N", "cancel");
    keys.use(Attr::bar).fill();
    panel.set(panel.height - 1, keys);
}

// The bar across the top: name and what it counts on the left, `right` at the
// right edge when both fit.
void title_bar(Line &line, std::string_view what, const Line &right) {
    line.use(Attr::bar_key).text(" WatchTasks ");
    line.use(Attr::bar).text(what);
    if (line.room() >= right.length) {
        line.spaces(line.room() - right.length);
        std::ranges::copy(right.view(), line.data.begin() + line.length);
        std::ranges::copy(std::span{right.attr}.first(right.length),
                          line.attr.begin() + line.length);
        line.length += right.length;
    }
    line.use(Attr::bar).fill();
}

void help_item(Line &line, const HelpItem &item, unsigned width, unsigned key_width) {
    const unsigned start = line.length;
    if (item.section) {
        line.use(Attr::normal).character(' ');
        line.use(Attr::head).character(' ');
        line.text(item.key);
        line.character(' ');
    } else if (!item.key.empty()) {
        line.use(Attr::normal).text("   ");
        line.use(Attr::bold).field(item.key, key_width);
        line.use(Attr::normal).text(item.text);
    }
    line.use(Attr::normal);
    if (line.length < start + width) {
        line.spaces(start + width - line.length);
    }
}

void render_help(View &view, Screen &screen) {
    const unsigned page = help_page_rows(view.rows);
    const unsigned count = help_count(view.cols);
    view.help_scroll = std::min(view.help_scroll, count > page ? count - page : 0);
    const bool wide = view.cols >= wide_cols;

    Line range(view.cols, true);
    range.use(Attr::bar).text("Help ");
    range.decimal(view.help_scroll + 1);
    range.character('-');
    range.decimal(std::min(count, view.help_scroll + page));
    range.character('/');
    range.decimal(count);
    range.character(' ');
    Line title(view.cols, true);
    title_bar(title, "", range);
    screen.line(1, title);
    screen.line(2, Line(view.cols));

    for (unsigned i = 0; i < page; ++i) {
        const unsigned index = view.help_scroll + i;
        Line line(view.cols);
        if (index < count) {
            if (wide) {
                help_item(line, wide_help[index][0], view.cols / 2, 13);
                help_item(line, wide_help[index][1], line.room(), 7);
            } else {
                help_item(line, narrow_help[index], line.room(), 12);
            }
        }
        screen.line(3 + i, line);
    }

    Line footer(view.cols);
    static constexpr std::array<KeyItem, 4> help_keys = {
        {{"j/k", "scroll", 1}, {"n/u", "page", 2}, {"h", "back", 0}, {"q", "quit", 0}}};
    key_bar(footer, help_keys);
    screen.line(view.rows, footer);
    frame.flush();
}

constexpr std::string_view sort_name(char sort) {
    switch (sort) {
    case 'p':
        return "Priority";
    case 's':
        return "State";
    case 'c':
        return "CLI";
    case 'a':
        return "Name";
    default:
        return "CPU";
    }
}

// A column heading, in the sort colour when the table is sorted by it.
void heading(Line &line, std::string_view text, unsigned width, bool right, bool sorted) {
    line.use(sorted ? Attr::head_on : Attr::head);
    if (right) {
        line.right_field(text, width);
    } else {
        line.field(text, width);
    }
    line.use(Attr::head).character(' ');
}

void render(View &view, Screen &screen, LONG number) {
    screen.size(view.rows, view.cols);
    screen.panel = nullptr;
    if (view.rows < 14 || view.cols < 40) {
        Line line(view.cols);
        line.text("WatchTasks: resize window (40x14 minimum)");
        screen.line(1, line);
        frame.flush();
        return;
    }
    if (view.help) {
        render_help(view, screen);
        return;
    }
    prepare(view);
    if (view.confirming_break) {
        static Panel confirmation;
        render_break_confirmation(view, confirmation);
        screen.panel = &confirmation;
    } else if (view.details.active) {
        static Panel panel;
        render_details(view, panel);
        screen.panel = &panel;
    }
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
    const bool wide = view.cols >= wide_cols;
    const bool compact = view.cols < 60;

    // Title: counts on the left; sort, state and snapshot number on the right.
    Line counts(view.cols, true);
    counts.use(Attr::bar);
    counts.text(" ");
    counts.decimal(view.snapshot.count);
    counts.text(" tasks");
    if (!compact) {
        counts.text("  Run ");
        counts.decimal(running);
        counts.text("  Ready ");
        counts.decimal(ready);
        counts.text("  Wait ");
        counts.decimal(waiting);
    }
    Line right(view.cols, true);
    if (view.paused) {
        right.use(Attr::bar_key).text("PAUSED  ");
    }
    if (view.snapshot.truncated) {
        right.use(Attr::bar_key).text("TRUNCATED  ");
    }
    right.use(Attr::bar).text("Sort ");
    right.use(Attr::bar_key).text(sort_name(view.sort));
    if (!compact) {
        right.use(Attr::bar).text("  #");
        right.decimal(static_cast<ULONG>(number));
    }
    right.use(Attr::bar).character(' ');
    Line title(view.cols, true);
    title_bar(title, std::string_view{counts.data.data(), counts.length}, right);
    screen.line(1, title);

    // The machine: CPU busy, then chip and fast memory.
    Line cpu(view.cols);
    cpu.text("  ");
    cpu.field("CPU", 6);
    bar(cpu, view.cpu.samples ? view.cpu.busy_tenths : 0, 1000, meter_width(view.cols),
        Attr::fill);
    cpu.character(' ');
    cpu.use(Attr::bold);
    if (view.cpu.samples) {
        cpu.decimal(view.cpu.busy_tenths / 10, 5);
        cpu.character('.');
        cpu.decimal(view.cpu.busy_tenths % 10);
        cpu.character('%');
    } else {
        cpu.text("  --.-%");
    }
    if (cpu.room() >= 13) {
        cpu.use(Attr::normal).text("  ");
        cpu.decimal(view.cpu.samples);
        cpu.text(" samples");
    }
    screen.line(2, cpu);
    Line chip(view.cols);
    memory_meter(chip, "Chip", view.chip_free, view.chip_total, Attr::fill);
    screen.line(3, chip);
    Line fast(view.cols);
    memory_meter(fast, "Fast", view.fast_free, view.fast_total, Attr::fill_dark);
    screen.line(4, fast);

    // The filter while it is set or edited, and the last action's message.
    Line rule(view.cols);
    const std::string_view filter{view.filter.data()};
    const std::string_view message{view.editing ? "" : view.message};
    if (view.editing || !filter.empty()) {
        rule.use(Attr::normal).text("  ");
        rule.use(view.editing ? Attr::head : Attr::normal).text(" Filter ");
        rule.use(Attr::normal).character(' ');
        const unsigned reserved = static_cast<unsigned>(std::min<std::size_t>(
            rule.room(), (view.editing ? 1U : 0U) + (message.empty() ? 0U : message.size() + 4)));
        std::string_view shown_filter = filter;
        if (shown_filter.size() > rule.room() - reserved) {
            // substr() pulls in libstdc++'s exception runtime on this toolchain.
            shown_filter.remove_prefix(shown_filter.size() - (rule.room() - reserved));
        }
        rule.use(Attr::bold).text(shown_filter);
        if (view.editing) {
            rule.use(Attr::alert).character(' '); // the cursor
        }
    }
    if (!message.empty() && rule.room() > message.size() + 2) {
        rule.use(Attr::normal).text("  ");
        rule.use(Attr::alert).character(' ');
        rule.text(message);
        rule.character(' ');
    }
    screen.line(5, rule);

    // Column headings, a strip across the window.
    const unsigned cpu_bar = wide ? 10 : 0;
    Line headings(view.cols, true);
    headings.use(Attr::head).text("   T  ");
    if (!compact) {
        heading(headings, "TASK_ID", 8, false, false);
        heading(headings, "CLI", 4, true, view.sort == 'c');
        heading(headings, "PRI", 4, true, view.sort == 'p');
    }
    heading(headings, "CPU%", 6, true, view.sort == 't');
    if (cpu_bar) {
        headings.spaces(cpu_bar + 1);
    }
    if (!compact) {
        heading(headings, "STATE", 5, false, view.sort == 's');
    }
    heading(headings, "NAME", 4, false, view.sort == 'a');
    headings.use(Attr::head).fill();
    screen.line(6, headings);

    const unsigned page = task_page_rows(view.rows);
    for (unsigned i = 0; i < page; ++i) {
        const unsigned pos = view.scroll + i;
        Line line(view.cols, true);
        if (pos >= view.shown) {
            screen.line(7 + i, line);
            continue;
        }
        const unsigned task_index = view.order[pos];
        const TaskRow &row = view.snapshot.rows[task_index];
        const bool chosen = pos == view.selected;
        const Attr base = chosen ? Attr::select : Attr::normal;
        line.use(base);
        line.character(chosen ? '>' : ' ');
        line.spaces(2);
        line.use(chosen ? Attr::select : Attr::bold).character(row.type == NT_PROCESS ? 'P' : 'T');
        line.use(base).spaces(2);
        if (!compact) {
            line.hex8(row.address);
            line.character(' ');
            if (row.cli > 0) {
                line.decimal(static_cast<ULONG>(row.cli), 4);
            } else {
                line.right_field("-", 4);
            }
            line.character(' ');
            line.signed_decimal(row.priority, 4);
            line.character(' ');
        }
        line.use(chosen ? Attr::select : Attr::bold);
        cpu_field(line, view.cpu.tenths[task_index], view.cpu.samples);
        line.use(base).character(' ');
        if (cpu_bar) {
            const unsigned tenths = view.cpu.samples ? view.cpu.tenths[task_index] : 0;
            unsigned cells = (tenths * cpu_bar + 999) / 1000;
            cells = std::min(cells, cpu_bar);
            line.use(Attr::fill).spaces(cells);
            line.use(base).spaces(cpu_bar - cells + 1);
        }
        if (!compact) {
            line.field(shelltools::task_state(row.state), 5);
            line.character(' ');
        }
        line.use(chosen ? Attr::select : Attr::normal).text(row.name.data());
        if (chosen) {
            line.fill();
        }
        screen.line(7 + i, line);
    }

    // The selected task in two lines, labels plain and values bold.
    Line detail(view.cols);
    Line signals(view.cols);
    if (view.shown) {
        const TaskRow &row = view.snapshot.rows[view.order[view.selected]];
        Line n(20, true);
        n.decimal(view.selected + 1);
        n.character('/');
        n.decimal(view.shown);
        detail.pair("  Task ", std::string_view{n.data.data(), n.length});
        detail.pair("  ", row.type == NT_PROCESS ? "Process" : "Exec task");
        Line id(20, true);
        id.hex8(row.address);
        detail.pair("  ID ", std::string_view{id.data.data(), id.length});
        Line pri(20, true);
        pri.signed_decimal(row.priority);
        detail.pair("  Pri ", std::string_view{pri.data.data(), pri.length});
        if (row.cli > 0) {
            Line cli(20, true);
            cli.decimal(static_cast<ULONG>(row.cli));
            detail.pair("  CLI ", std::string_view{cli.data.data(), cli.length});
        }
        if (row.command.front() != '\0') {
            detail.pair("  Cmd ", row.command.data());
        }
        Line sig(30, true);
        sig.hex8(row.signal_alloc);
        sig.character('/');
        sig.hex8(row.signal_wait);
        sig.character('/');
        sig.hex8(row.signal_received);
        signals.pair("  Signals ", std::string_view{sig.data.data(), sig.length});
        Line stack(20, true);
        stack.hex8(row.stack_lower);
        stack.character('-');
        stack.hex8(row.stack_upper);
        signals.pair("  Stack ", std::string_view{stack.data.data(), stack.length});
    } else {
        detail.text("  No matching tasks");
    }
    screen.line(view.rows - 2, detail);
    screen.line(view.rows - 1, signals);

    Line shortcuts(view.cols);
    static constexpr std::array<KeyItem, 7> table_keys = {
        {{"Enter", "info", 0}, {"j/k", "move", 2}, {"t/p/s/c/a", "sort", 3},
         {"/", "filter", 1}, {"b", "break", 3}, {"h", "help", 0}, {"q", "quit", 0}}};
    key_bar(shortcuts, table_keys);
    screen.line(view.rows, shortcuts);
    frame.flush();
}

struct RawConsole {
    BPTR input;
    bool active;
    unsigned rows = 24;
    unsigned cols = 80;
    bool size_known = false;
    bool size_changed = false;
    bool size_pending = false;
    unsigned size_polls = 0;
    std::array<char, 40> sequence = {};
    unsigned sequence_length = 0;
    unsigned escape = 0;
    std::array<char, 16> keys = {};
    unsigned head = 0;
    unsigned tail = 0;

    explicit RawConsole(BPTR in) : input(in), active(SetMode(in, 1) != 0) {}
    ~RawConsole() {
        if (active) {
            // Consume our outstanding reply before returning the input to the Shell.
            unsigned idle = 0;
            for (unsigned i = 0; i < sequence.size() && idle < 10 && size_pending; ++i) {
                idle = poll(20000) ? 0 : idle + 1;
            }
            // Clear, wrap and scroll back on, cursor back on.
            frame.put("\2330m\f\233?7h\233>1h\233 p");
            frame.flush();
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
        size_pending = false;
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
        if (size_pending && ++size_polls >= 100) {
            size_pending = false;
        }
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

    void request_size() {
        // Short refresh intervals must not queue requests faster than replies are read.
        if (size_pending) {
            return;
        }
        size_pending = true;
        size_polls = 0;
        frame.put("\2330 q");
        frame.flush();
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
    if (view.confirming_break) {
        if (view.rows < 14 || view.cols < 40) {
            view.confirming_break = false;
            view.message = "Break cancelled: window too small";
            return;
        }
        if (key == 'y' || key == 'Y') {
            const bool sent = !view.break_target_missing && break_selected(view.break_target);
            view.message = sent ? "Ctrl-C sent" : "Task changed or missing";
            view.confirming_break = false;
        } else if (key == 'n' || key == 'N' || key == '\r' || key == '\n' || key == 27 ||
                   key == 'q' || key == 'Q') {
            view.confirming_break = false;
            view.message = "Break cancelled";
        }
        return;
    }
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
        view.selection_valid = false;
        view.scroll = 0;
        return;
    }
    if (view.details.active) {
        TaskDetails &details = view.details;
        const unsigned page = details_page_rows(view.rows);
        const unsigned end = details.lines > page ? details.lines - page : 0;
        details.scroll = std::min(details.scroll, end);
        if (key == 'q' || key == 'Q') {
            done = true;
        } else if (key == '\r' || key == '\n' || key == 27) {
            details.active = false;
            for (unsigned i = 0; i < view.shown; ++i) {
                if (same_task(view.snapshot.rows[view.order[i]], details.row)) {
                    view.selected = i;
                    break;
                }
            }
        } else if (key == 'j' && details.scroll < end) {
            ++details.scroll;
        } else if (key == 'k' && details.scroll) {
            --details.scroll;
        } else if (key == 'n') {
            details.scroll += std::min(page, end - details.scroll);
        } else if (key == 'u') {
            details.scroll -= std::min(page, details.scroll);
        }
        return;
    }
    if (view.help) {
        const unsigned page = help_page_rows(view.rows);
        const unsigned count = help_count(view.cols);
        const unsigned end = count > page ? count - page : 0;
        view.help_scroll = std::min(view.help_scroll, end);
        if (key == 'q' || key == 'Q') {
            done = true;
        } else if (key == 'h' || key == 'H' || key == '?' || key == 27) {
            view.help = false;
        } else if (key == 'j' && view.help_scroll < end) {
            ++view.help_scroll;
        } else if (key == 'k' && view.help_scroll) {
            --view.help_scroll;
        } else if (key == 'n') {
            view.help_scroll += std::min(page, end - view.help_scroll);
        } else if (key == 'u') {
            view.help_scroll -= std::min(page, view.help_scroll);
        }
        return;
    }
    if (key == 'j' || key == 'k' || key == 'n' || key == 'u' || key == '/' ||
        key == 't' || key == 'p' || key == 's' || key == 'c' || key == 'a') {
        view.selection_valid = false;
    }
    if (key == 'q' || key == 'Q') {
        done = true;
    } else if ((key == '\r' || key == '\n') && view.shown) {
        const unsigned index = view.order[view.selected];
        view.details.row = view.snapshot.rows[index];
        view.details.cpu_tenths = view.cpu.tenths[index];
        view.details.samples = view.cpu.samples;
        view.details.scroll = 0;
        view.details.active = true;
    } else if (key == 'h' || key == 'H' || key == '?') {
        view.help = true;
        view.help_scroll = 0;
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
        view.selected += task_page_rows(view.rows);
    } else if (key == 'u') {
        view.selected = view.selected > task_page_rows(view.rows)
                            ? view.selected - task_page_rows(view.rows)
                            : 0;
    } else if (key == 't' || key == 'p' || key == 's' || key == 'c' || key == 'a') {
        view.sort = key;
        view.selected = 0;
        view.scroll = 0;
    } else if (key == 'b' || key == 'B') {
        if (view.rows < 14 || view.cols < 40) {
            view.message = "Resize window before break";
        } else if (view.shown) {
            view.break_target = view.snapshot.rows[view.order[view.selected]];
            view.break_target_missing = false;
            view.confirming_break = true;
        } else {
            view.message = "No task selected";
        }
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
        console.request_size();
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
        for (LONG remaining = ticks; remaining > 0;) {
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
                return shelltools::warn;
            }
            const LONG slice = std::min<LONG>(remaining, 10);
            Delay(slice);
            remaining -= slice;
        }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            return shelltools::warn;
        }
        shelltools::capture_tasks(snapshot);
        cpu.update(sampler, snapshot);
        if (machine) {
            Printf("{\"snapshot\":%ld,\"chip_free\":%lu,\"fast_free\":%lu}\n", round + 1,
                   AvailMem(MEMF_CHIP), AvailMem(MEMF_FAST));
        } else {
            Printf("SNAPSHOT=%ld CHIP_FREE=%lu FAST_FREE=%lu\n", round + 1, AvailMem(MEMF_CHIP),
                   AvailMem(MEMF_FAST));
            shelltools::print_task_header(true);
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

constexpr char version_tag[] = SHELLTOOLS_VERSTAG("WatchTasks");

[[gnu::noinline, gnu::noipa]] int watch_main() {
    LONG args[6] = {};
    RDArgs *rd = ReadArgs("TICKS/N/K,COUNT/N/K,ROWS/N/K,COLS/N/K,MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("WatchTasks");
    }
    if (args[5]) {
        shelltools::help(version_tag,
            "PURPOSE: Live task CPU and memory monitor\n"
            "USAGE: WatchTasks [TICKS=<n>] [COUNT=<n>] [ROWS=<n>] [COLS=<n>] [MACHINE]\n"
            "DEFAULT: 50 ticks, unlimited, auto console size\n"
            "T/P: T=Exec task (thread), P=DOS process\n"
            "KEYS: Enter details; j/k move; t/p/s/c/a sort; / filter; b break; h help; q quit\n"
            "DETAILS: Enter/Esc close; j/k scroll; n/u page\n"
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
    const int result = !machine && IsInteractive(Input()) && IsInteractive(Output())
                           ? watch_interactive(ticks, count, static_cast<unsigned>(rows),
                                               static_cast<unsigned>(cols))
                           : watch_snapshots(ticks, count, machine);
    return shelltools::break_requested() ? shelltools::break_result("WatchTasks") : result;
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
    shelltools::arm_break();
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
