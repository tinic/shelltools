#include "tool_support.hpp"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

namespace {

// identify.library public API: IdHardware(type, tags), LVO -36.
// Kept optional so the command also runs on a plain AmigaOS installation.
const char *id_hardware(Library *library, ULONG type) {
    register Library *base __asm("a6") = library;
    register ULONG value __asm("d0") = type;
    register APTR tags __asm("a0") = nullptr;
    __asm volatile("jsr %%a6@(-36:W)"
                   : "+d"(value)
                   : "a"(base), "a"(tags)
                   : "cc", "memory", "d1", "a1");
    return reinterpret_cast<const char *>(value);
}

struct Field {
    const char *key;
    ULONG id;
};

constexpr Field fields[] = {{"system", 0},   {"cpu", 1},   {"fpu", 2},     {"mmu", 3},
                            {"os", 15},      {"exec", 5},  {"chipset", 8}, {"cpu_clock", 35},
                            {"graphics", 9}, {"audio", 14}};

} // namespace

int main() {
    shelltools::arm_break();
    LONG args[2] = {};
    RDArgs *rd = ReadArgs("MACHINE/S,HELP/S", args, nullptr);
    if (rd == nullptr) {
        return shelltools::dos_error("SystemInfo");
    }
    if (args[1]) {
        shelltools::help("PURPOSE: Identify AmigaOS system and hardware\n"
                         "USAGE: SystemInfo [MACHINE]\n"
                         "SOURCE: identify.library when installed; Exec fallback\n"
                         "OUTPUT: Key/value rows; MACHINE gives JSON\n"
                         "RC: 0=ok 5=break 10=error\n");
        FreeArgs(rd);
        return shelltools::ok;
    }
    const bool machine = args[0] != 0;
    FreeArgs(rd);

    if (shelltools::break_requested()) {
        return shelltools::break_result("SystemInfo");
    }

    Library *identify = OpenLibrary("identify.library", 8);
    if (machine) {
        PutStr("{\"identify_library\":");
        PutStr(identify ? "true" : "false");
        Printf(",\"exec_version\":%lu,\"exec_revision\":%lu,\"chip_free\":%lu,\"fast_free\":%lu",
               static_cast<ULONG>(SysBase->LibNode.lib_Version),
               static_cast<ULONG>(SysBase->LibNode.lib_Revision), AvailMem(MEMF_CHIP),
               AvailMem(MEMF_FAST));
        if (identify) {
            for (const Field &field : fields) {
                if (shelltools::break_requested()) {
                    CloseLibrary(identify);
                    return shelltools::break_result("SystemInfo");
                }
                PutStr(",\"");
                PutStr(field.key);
                PutStr("\":");
                const char *value = id_hardware(identify, field.id);
                if (value) {
                    shelltools::write_json_string(Output(), value);
                } else {
                    PutStr("null");
                }
            }
        }
        PutStr("}\n");
    } else {
        Printf("IDENTIFY_LIBRARY=%s\nEXEC=%lu.%lu\nCHIP_FREE=%lu\nFAST_FREE=%lu\n",
               identify ? "yes" : "no", static_cast<ULONG>(SysBase->LibNode.lib_Version),
               static_cast<ULONG>(SysBase->LibNode.lib_Revision), AvailMem(MEMF_CHIP),
               AvailMem(MEMF_FAST));
        if (identify) {
            for (const Field &field : fields) {
                if (shelltools::break_requested()) {
                    CloseLibrary(identify);
                    return shelltools::break_result("SystemInfo");
                }
                const char *value = id_hardware(identify, field.id);
                if (value) {
                    Printf("%s=%s\n", field.key, value);
                }
            }
        }
    }
    if (identify) {
        CloseLibrary(identify);
    }
    return shelltools::break_requested() ? shelltools::break_result("SystemInfo") : shelltools::ok;
}
