#include "tool_support.hpp"

int main() {
    LONG args[3] = {};
    RDArgs *rd = ReadArgs("PATH,MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr)
        return shelltools::dos_error("DiskFree");
    if (args[2]) {
        shelltools::help("PURPOSE: Show filesystem capacity for a path\n"
                         "USAGE: DiskFree [PATH] [MACHINE]\n"
                         "DEFAULT: SYS:\n"
                         "OUTPUT: Table; MACHINE gives JSON\n"
                         "RC: 0=ok 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const char *path = args[0] ? reinterpret_cast<const char *>(args[0]) : "SYS:";
    const bool machine = args[1] != 0;
    const BPTR lock = Lock(path, ACCESS_READ);
    if (lock == 0) {
        const int rc = shelltools::dos_error("DiskFree");
        FreeArgs(rd);
        return rc;
    }
    static InfoData info;
    const BOOL success = Info(lock, &info);
    UnLock(lock);
    if (!success) {
        const int rc = shelltools::dos_error("DiskFree");
        FreeArgs(rd);
        return rc;
    }
    const ULONG total = static_cast<ULONG>(info.id_NumBlocks);
    const ULONG used = static_cast<ULONG>(info.id_NumBlocksUsed);
    const ULONG free_blocks = used <= total ? total - used : 0;
    if (machine) {
        PutStr("{\"path\":");
        if (!shelltools::write_json_string(Output(), path)) {
            FreeArgs(rd);
            return shelltools::dos_error("DiskFree");
        }
        Printf(",\"blocks\":%lu,\"used\":%lu,\"free\":%lu,\"bytes_per_block\":%lu,\"state\":%ld,"
               "\"type\":\"%08lx\"}\n",
               total, used, free_blocks, static_cast<ULONG>(info.id_BytesPerBlock),
               info.id_DiskState, static_cast<ULONG>(info.id_DiskType));
    } else {
        PutStr("PATH BLOCKS USED FREE BYTES_PER_BLOCK STATE TYPE\n");
        Printf("%s %lu %lu %lu %lu %ld %08lx\n", path, total, used, free_blocks,
               static_cast<ULONG>(info.id_BytesPerBlock), info.id_DiskState,
               static_cast<ULONG>(info.id_DiskType));
    }
    FreeArgs(rd);
    return shelltools::ok;
}
