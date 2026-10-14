#include "tool_support.hpp"

#include <exec/execbase.h>
#include <proto/exec.h>

int main() {
    LONG args[1] = {};
    RDArgs *rd = ReadArgs("HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("RebootSync");
    if (args[0]) {
        shelltools::help("PURPOSE: Reboot after partition sync\n"
                         "USAGE: RebootSync\n"
                         "REQUIRES: AmigaOS 3.2 C:Reboot SYNC\n"
                         "RC: 10=unsupported/failed; success reboots\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    FreeArgs(rd);
    if (SysBase->LibNode.lib_Version < 47) {
        PutStr("RebootSync: requires AmigaOS 3.2 Reboot SYNC\n");
        return shelltools::error;
    }
    const LONG result = SystemTagList("C:Reboot SYNC", nullptr);
    if (result == -1) {
        PutStr("RebootSync: failed to launch C:Reboot\n");
        return shelltools::error;
    }
    if (result != 0) {
        Printf("RebootSync: Reboot returned %ld\n", result);
        return shelltools::error;
    }
    return shelltools::ok;
}
