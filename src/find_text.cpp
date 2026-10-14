#include "tool_support.hpp"

#include <exec/memory.h>
#include <proto/exec.h>

namespace {

int fold(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool contains(const char *line, ULONG length, const char *pattern, ULONG pattern_length,
              bool nocase) {
    if (pattern_length > length)
        return false;
    for (ULONG at = 0; at + pattern_length <= length; ++at) {
        ULONG i = 0;
        while (i < pattern_length) {
            const int left = static_cast<UBYTE>(line[at + i]);
            const int right = static_cast<UBYTE>(pattern[i]);
            if ((nocase ? fold(left) : left) != (nocase ? fold(right) : right))
                break;
            ++i;
        }
        if (i == pattern_length)
            return true;
    }
    return false;
}

ULONG length_of(const char *s) {
    ULONG n = 0;
    while (s[n])
        ++n;
    return n;
}

} // namespace

int main() {
    LONG args[7] = {};
    RDArgs *rd = ReadArgs("TEXT,FILE,NOCASE/S,NUMBER/S,INVERT/S,COUNT/S,HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("FindText");
    if (args[6]) {
        shelltools::help("PURPOSE: Find literal text in file lines\n"
                         "USAGE: FindText TEXT [FILE] [NOCASE] [NUMBER] [INVERT] [COUNT]\n"
                         "DEFAULT: input stream when FILE omitted\n"
                         "RC: 0=matched 5=no match 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    if (args[0] == 0) {
        PutStr("FindText: TEXT required; use HELP\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    const char *pattern = reinterpret_cast<const char *>(args[0]);
    const ULONG pattern_length = length_of(pattern);
    const bool own_file = args[1] != 0;
    BPTR file = own_file ? Open(reinterpret_cast<const char *>(args[1]), MODE_OLDFILE) : Input();
    if (file == 0) {
        const int rc = shelltools::dos_error("FindText");
        FreeArgs(rd);
        return rc;
    }
    constexpr ULONG maximum_line = 65536;
    ULONG capacity = 1024;
    auto *line = static_cast<char *>(AllocVec(capacity, MEMF_ANY));
    if (line == nullptr) {
        if (own_file)
            Close(file);
        FreeArgs(rd);
        PutStr("FindText: out of memory\n");
        return shelltools::error;
    }
    static char input[4096];
    ULONG length = 0;
    ULONG line_number = 0;
    ULONG matches = 0;
    int rc = shelltools::ok;
    bool at_eof = false;
    while (!at_eof && rc == shelltools::ok) {
        const LONG count = Read(file, input, sizeof input);
        if (count < 0) {
            rc = shelltools::dos_error("FindText");
            break;
        }
        at_eof = count == 0;
        for (LONG j = 0; j <= count; ++j) {
            if (j == count && !at_eof)
                break;
            const bool end = j == count || input[j] == '\n';
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
                    for (ULONG i = 0; i < length; ++i)
                        next[i] = line[i];
                    FreeVec(line);
                    line = next;
                    capacity = next_capacity;
                }
                line[length++] = input[j];
                continue;
            }
            if (j == count && length == 0)
                break;
            ++line_number;
            bool selected = contains(line, length, pattern, pattern_length, args[2] != 0);
            if (args[4])
                selected = !selected;
            if (selected) {
                ++matches;
                if (!args[5]) {
                    if (args[3])
                        Printf("%lu:", line_number);
                    if (!shelltools::write_all(Output(), line, length) ||
                        !shelltools::write_all(Output(), "\n", 1)) {
                        rc = shelltools::dos_error("FindText");
                        break;
                    }
                }
            }
            length = 0;
        }
    }
    if (rc == shelltools::ok && args[5])
        Printf("%lu\n", matches);
    FreeVec(line);
    if (own_file)
        Close(file);
    FreeArgs(rd);
    return rc == shelltools::ok && matches == 0 ? shelltools::warn : rc;
}
