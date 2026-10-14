#include "tool_support.hpp"

int main() {
    LONG args[3] = {};
    RDArgs *rd = ReadArgs("FILE,LINES/N/K,HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("LastLines");
    if (args[2]) {
        shelltools::help("PURPOSE: Print last lines of a seekable file\n"
                         "USAGE: LastLines FILE [LINES=<n>]\n"
                         "DEFAULT: 10 lines\n"
                         "RC: 0=ok 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    if (args[0] == 0) {
        PutStr("LastLines: FILE required; use HELP\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    const LONG limit = args[1] ? *reinterpret_cast<LONG *>(args[1]) : 10;
    if (limit < 0) {
        PutStr("LastLines: LINES must be nonnegative\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    BPTR file = Open(reinterpret_cast<const char *>(args[0]), MODE_OLDFILE);
    if (file == 0) {
        const int rc = shelltools::dos_error("LastLines");
        FreeArgs(rd);
        return rc;
    }
    int rc = shelltools::ok;
    const LONG seek_rc = Seek(file, 0, OFFSET_END);
    const LONG size = seek_rc < 0 ? -1 : Seek(file, 0, OFFSET_CURRENT);
    if (size < 0)
        rc = shelltools::dos_error("LastLines");
    static char buffer[4096];
    LONG start = 0;
    LONG remaining = size;
    LONG lines = 0;
    bool found = false;
    while (rc == shelltools::ok && limit > 0 && remaining > 0 && !found) {
        const LONG count = remaining < static_cast<LONG>(sizeof buffer)
                               ? remaining
                               : static_cast<LONG>(sizeof buffer);
        const LONG offset = remaining - count;
        if (Seek(file, offset, OFFSET_BEGINNING) < 0 || Read(file, buffer, count) != count) {
            rc = shelltools::dos_error("LastLines");
            break;
        }
        for (LONG i = count; i > 0; --i) {
            const LONG absolute = offset + i - 1;
            if (buffer[i - 1] != '\n' || absolute == size - 1)
                continue;
            if (++lines == limit) {
                start = absolute + 1;
                found = true;
                break;
            }
        }
        remaining = offset;
    }
    if (rc == shelltools::ok && limit > 0) {
        if (Seek(file, start, OFFSET_BEGINNING) < 0) {
            rc = shelltools::dos_error("LastLines");
        } else {
            LONG count;
            while ((count = Read(file, buffer, sizeof buffer)) > 0) {
                if (!shelltools::write_all(Output(), buffer, count)) {
                    rc = shelltools::dos_error("LastLines");
                    break;
                }
            }
            if (count < 0)
                rc = shelltools::dos_error("LastLines");
        }
    }
    Close(file);
    FreeArgs(rd);
    return rc;
}
