#include "tool_support.hpp"

#include <exec/memory.h>
#include <libregexp.h>
#include <proto/exec.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

namespace {

struct RegexContext {
    static constexpr unsigned max_polls = 20;
    unsigned polls = 0;
    bool interrupted = false;
};

} // namespace

extern "C" bool lre_check_stack_overflow(void *opaque, size_t alloca_size) {
    (void)opaque;
    char marker = 0;
    const auto pointer = reinterpret_cast<std::uintptr_t>(&marker);
    const auto lower = reinterpret_cast<std::uintptr_t>(SysBase->ThisTask->tc_SPLower);
    return pointer < lower || pointer - lower <= 2048U + alloca_size;
}

extern "C" int lre_check_timeout(void *opaque) {
    auto &context = *static_cast<RegexContext *>(opaque);
    context.interrupted = (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
    return context.interrupted || ++context.polls > RegexContext::max_polls;
}

extern "C" void *lre_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) {
        std::free(ptr);
        return nullptr;
    }
    return std::realloc(ptr, size);
}

namespace {

struct Options {
    std::string_view pattern{};
    const char *file_name = nullptr;
    bool nocase = false;
    bool number = false;
    bool invert = false;
    bool count = false;
    bool regex = false;
};

Options regex_options{};

class Regex {
  public:
    Regex() = default;
    Regex(const Regex &) = delete;
    Regex &operator=(const Regex &) = delete;
    ~Regex() {
        std::free(captures_);
        std::free(bytecode_);
    }

    bool compile(std::string_view pattern, bool nocase) {
        int bytecode_length = 0;
        const int flags = nocase ? LRE_FLAG_IGNORECASE : 0;
        bytecode_ = lre_compile(&bytecode_length, error_.data(), static_cast<int>(error_.size()),
                                pattern.data(), pattern.size(), flags, &context_);
        if (bytecode_ == nullptr) {
            return false;
        }
        const int slots = lre_get_alloc_count(bytecode_);
        if (slots <= 0) {
            return false;
        }
        captures_ = static_cast<std::uint8_t **>(
            std::malloc(static_cast<std::size_t>(slots) * sizeof(*captures_)));
        return captures_ != nullptr;
    }

    int match(std::span<const char> line) {
        context_.polls = 0;
        context_.interrupted = false;
        return lre_exec(captures_, bytecode_, reinterpret_cast<const std::uint8_t *>(line.data()),
                        0, static_cast<int>(line.size()), 0, &context_);
    }

    const char *error() const { return error_.front() != '\0' ? error_.data() : "out of memory"; }
    bool interrupted() const { return context_.interrupted; }

  private:
    RegexContext context_{};
    std::array<char, 128> error_{};
    std::uint8_t *bytecode_ = nullptr;
    std::uint8_t **captures_ = nullptr;
};

constexpr int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

constexpr bool contains(std::span<const char> line, std::string_view pattern, bool nocase) {
    if (pattern.empty()) {
        return true;
    }
    const auto matches = [nocase](char a, char b) {
        const auto left = static_cast<UBYTE>(a);
        const auto right = static_cast<UBYTE>(b);
        return (nocase ? fold(left) : left) == (nocase ? fold(right) : right);
    };
    return std::ranges::search(line, pattern, matches).begin() != line.end();
}

[[gnu::noinline, gnu::noipa]] int find_text() {
    const Options &options = regex_options;
    if (options.regex && options.pattern.size() > 1024) {
        PutStr("FindText: regexp exceeds 1024 bytes\n");
        return shelltools::error;
    }
    Regex regex;
    if (options.regex && !regex.compile(options.pattern, options.nocase)) {
        Printf("FindText: regexp: %s\n", regex.error());
        return shelltools::error;
    }
    const bool own_file = options.file_name != nullptr;
    BPTR file = own_file ? Open(options.file_name, MODE_OLDFILE) : Input();
    if (file == 0) {
        return shelltools::dos_error("FindText");
    }
    constexpr ULONG maximum_line = 65536;
    ULONG capacity = 1024;
    auto *line = static_cast<char *>(AllocVec(capacity, MEMF_ANY));
    if (line == nullptr) {
        if (own_file) {
            Close(file);
        }
        PutStr("FindText: out of memory\n");
        return shelltools::error;
    }
    static std::array<char, 4096> input;
    ULONG length = 0;
    ULONG line_number = 0;
    ULONG matches = 0;
    int rc = shelltools::ok;
    bool at_eof = false;
    while (!at_eof && rc == shelltools::ok) {
        const LONG count = Read(file, input.data(), input.size());
        if (count < 0) {
            rc = shelltools::dos_error("FindText");
            break;
        }
        at_eof = count == 0;
        for (LONG j = 0; j <= count; ++j) {
            if (j == count && !at_eof) {
                break;
            }
            const auto index = static_cast<std::size_t>(j);
            const bool end = j == count || input[index] == '\n';
            if (!end) {
                if (length == capacity) {
                    if (capacity == maximum_line) {
                        PutStr("FindText: line exceeds 65536 bytes\n");
                        rc = shelltools::error;
                        break;
                    }
                    const ULONG next_capacity = capacity * 2;
                    auto *next = static_cast<char *>(AllocVec(next_capacity, MEMF_ANY));
                    if (next == nullptr) {
                        PutStr("FindText: out of memory\n");
                        rc = shelltools::error;
                        break;
                    }
                    std::copy_n(line, length, next);
                    FreeVec(line);
                    line = next;
                    capacity = next_capacity;
                }
                line[length++] = input[index];
                continue;
            }
            if (j == count && length == 0) {
                break;
            }
            ++line_number;
            const int result =
                options.regex ? regex.match(std::span{line, length})
                              : contains(std::span{line, length}, options.pattern, options.nocase);
            if (result < 0) {
                PutStr(regex.interrupted()         ? "FindText: interrupted\n"
                       : result == LRE_RET_TIMEOUT ? "FindText: regexp work limit reached\n"
                                                   : "FindText: regexp out of memory or invalid\n");
                rc = shelltools::error;
                break;
            }
            bool selected = result != 0;
            if (options.invert) {
                selected = !selected;
            }
            if (selected) {
                ++matches;
                if (!options.count) {
                    if (options.number) {
                        Printf("%lu:", line_number);
                    }
                    if (!shelltools::write_all(Output(), line, static_cast<LONG>(length)) ||
                        !shelltools::write_all(Output(), "\n", 1)) {
                        rc = shelltools::dos_error("FindText");
                        break;
                    }
                }
            }
            length = 0;
        }
    }
    if (rc == shelltools::ok && options.count) {
        Printf("%lu\n", matches);
    }
    FreeVec(line);
    if (own_file) {
        Close(file);
    }
    return rc == shelltools::ok && matches == 0 ? shelltools::warn : rc;
}

constexpr ULONG regex_stack_size = 16UL * 1024UL;
StackSwapStruct regex_stack_swap = {};
int regex_result = shelltools::error;

// Keep this frame free of locals: it spans both StackSwap calls.
[[gnu::noinline, gnu::noipa]] void regex_trampoline() {
    StackSwap(&regex_stack_swap);
    regex_result = find_text();
    StackSwap(&regex_stack_swap);
}

} // namespace

int main() {
    LONG args[8] = {};
    RDArgs *rd =
        ReadArgs("TEXT,FILE,NOCASE/S,NUMBER/S,INVERT/S,COUNT/S,REGEX/S,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("FindText");
    }
    if (args[7]) {
        shelltools::help("PURPOSE: Find text in file lines\n"
                         "USAGE: FindText TEXT [FILE] [REGEX] [NOCASE] [NUMBER] [INVERT] [COUNT]\n"
                         "DEFAULT: Literal search of input stream\n"
                         "REGEX: QuickJS syntax; pattern up to 1024 bytes\n"
                         "RC: 0=matched 5=no match 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    if (args[0] == 0) {
        PutStr("FindText: TEXT required; use HELP\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    const Options options{std::string_view{reinterpret_cast<const char *>(args[0])},
                          args[1] ? reinterpret_cast<const char *>(args[1]) : nullptr,
                          args[2] != 0,
                          args[3] != 0,
                          args[4] != 0,
                          args[5] != 0,
                          args[6] != 0};
    int result = shelltools::error;
    regex_options = options;
    if (!options.regex) {
        result = find_text();
    } else {
        APTR stack = AllocMem(regex_stack_size, MEMF_ANY);
        if (stack == nullptr) {
            PutStr("FindText: no memory for regexp stack\n");
        } else {
            auto *upper = static_cast<UBYTE *>(stack) + regex_stack_size;
            regex_stack_swap.stk_Lower = stack;
            regex_stack_swap.stk_Upper = reinterpret_cast<ULONG>(upper);
            regex_stack_swap.stk_Pointer = upper;
            regex_trampoline();
            result = regex_result;
            FreeMem(stack, regex_stack_size);
        }
    }
    FreeArgs(rd);
    return result;
}
