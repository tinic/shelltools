#pragma once

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <array>
#include <string_view>

// The $VER: tag AmigaOS Version reads.  CMake defines all three; a
// distribution sets SHELLTOOLS_VERSION_SUFFIX to append its own marker.
#ifndef SHELLTOOLS_VERSION
#define SHELLTOOLS_VERSION "1.0"
#endif
#ifndef SHELLTOOLS_VERSION_DATE
#define SHELLTOOLS_VERSION_DATE ""
#endif
#ifndef SHELLTOOLS_VERSION_SUFFIX
#define SHELLTOOLS_VERSION_SUFFIX ""
#endif
#define SHELLTOOLS_VERSTAG(name) \
    "$VER: " name " " SHELLTOOLS_VERSION SHELLTOOLS_VERSION_DATE SHELLTOOLS_VERSION_SUFFIX

namespace shelltools {

inline constexpr int ok = 0;
inline constexpr int warn = 5;
inline constexpr int error = 10;

inline void arm_break() {
    SetSignal(0, SIGBREAKF_CTRL_C);
}

inline bool break_requested() {
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

inline int break_result(const char *command) {
    PrintFault(ERROR_BREAK, const_cast<char *>(command));
    return warn;
}

constexpr bool parse_task_id(std::string_view text, ULONG &address) {
    if (text.starts_with('$')) {
        text.remove_prefix(1);
    } else if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    address = 0;
    for (const char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
        if (address > 0x0fffffffUL) {
            return false;
        }
        address = (address << 4) | digit;
    }
    return address != 0;
}

inline bool write_all(BPTR out, const char *data, LONG count) {
    while (count > 0) {
        // FWrite shares DOS's buffered output path with Printf and PutStr.
        // Write bypasses it and can reorder fragments of a JSON record.
        const LONG wrote = FWrite(out, data, 1, static_cast<ULONG>(count));
        if (wrote <= 0) {
            return false;
        }
        data += wrote;
        count -= wrote;
    }
    return true;
}

inline bool write_json_string(BPTR out, std::string_view input) {
    static constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, 128> buffer;
    unsigned n = 0;
    buffer[n++] = '"';
    for (const char character : input) {
        const UBYTE c = static_cast<UBYTE>(character);
        if (n + 6 >= buffer.size()) {
            if (!write_all(out, buffer.data(), static_cast<LONG>(n))) {
                return false;
            }
            n = 0;
        }
        if (c == '"' || c == '\\') {
            buffer[n++] = '\\';
            buffer[n++] = static_cast<char>(c);
        } else if (c < 0x20 || c >= 0x80) {
            buffer[n++] = '\\';
            buffer[n++] = 'u';
            buffer[n++] = '0';
            buffer[n++] = '0';
            buffer[n++] = hex[c >> 4];
            buffer[n++] = hex[c & 15];
        } else {
            buffer[n++] = static_cast<char>(c);
        }
    }
    buffer[n++] = '"';
    return write_all(out, buffer.data(), static_cast<LONG>(n));
}

inline int dos_error(const char *command) {
    PrintFault(IoErr(), const_cast<char *>(command));
    return error;
}

// HELP starts with the version line.  The reference also keeps the $VER:
// tag through --gc-sections.
inline void help(const char *version_tag, const char *text) {
    PutStr(const_cast<char *>(version_tag + 6));
    PutStr(const_cast<char *>("\n"));
    PutStr(const_cast<char *>(text));
}

} // namespace shelltools
