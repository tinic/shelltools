#include "tool_support.hpp"

int main() {
    shelltools::arm_break();
    LONG args[3] = {};
    RDArgs *rd = ReadArgs("FILE,LINES/N/K,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("FirstLines");
    }
    if (args[2]) {
        shelltools::help("PURPOSE: Print first lines of a file or input\n"
                         "USAGE: FirstLines [FILE] [LINES=<n>]\n"
                         "DEFAULT: 10 lines; input stream when FILE omitted\n"
                         "RC: 0=ok 5=break 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const LONG limit = args[1] ? *reinterpret_cast<LONG *>(args[1]) : 10;
    if (limit < 0) {
        PutStr("FirstLines: LINES must be nonnegative\n");
        FreeArgs(rd);
        return shelltools::error;
    }
    const bool own_file = args[0] != 0;
    BPTR file = own_file ? Open(reinterpret_cast<const char *>(args[0]), MODE_OLDFILE) : Input();
    if (file == 0) {
        const int rc = shelltools::dos_error("FirstLines");
        FreeArgs(rd);
        return rc;
    }
    static char buffer[4096];
    LONG lines = 0;
    int rc = shelltools::ok;
    while (lines < limit) {
        if (!shelltools::wait_for_input(file)) {
            rc = shelltools::break_result("FirstLines");
            break;
        }
        const LONG count = Read(file, buffer, sizeof buffer);
        if (shelltools::break_requested()) {
            rc = shelltools::break_result("FirstLines");
            break;
        }
        if (count < 0) {
            rc = IoErr() == ERROR_BREAK ? shelltools::break_result("FirstLines")
                                         : shelltools::dos_error("FirstLines");
            break;
        }
        if (count == 0) {
            break;
        }
        LONG n = 0;
        while (n < count && lines < limit) {
            if ((n & 255) == 0 && shelltools::break_requested()) {
                rc = shelltools::break_result("FirstLines");
                break;
            }
            if (buffer[n] == '\n') {
                ++lines;
            }
            ++n;
        }
        if (rc != shelltools::ok) {
            break;
        }
        if (!shelltools::write_all(Output(), buffer, n)) {
            rc = IoErr() == ERROR_BREAK || shelltools::break_requested()
                     ? shelltools::break_result("FirstLines")
                     : shelltools::dos_error("FirstLines");
            break;
        }
    }
    if (own_file) {
        Close(file);
    }
    FreeArgs(rd);
    return rc == shelltools::ok && shelltools::break_requested()
               ? shelltools::break_result("FirstLines")
               : rc;
}
