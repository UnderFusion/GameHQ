// Child process for tst_crashdump: installs CrashDump into argv[1], then dies
// the way argv[2] says ("av" = access violation, "terminate" = an uncaught C++
// exception). Windows' crash dialog is suppressed so the test never shows UI.
#include "diagnostics/CrashDump.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <windows.h>

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 3)
        return 2;
    CrashDump::install(QString::fromLocal8Bit(argv[1]));
    if (std::strcmp(argv[2], "av") == 0) {
        volatile int* nowhere = nullptr;
        *nowhere = 42;
    } else if (std::strcmp(argv[2], "terminate") == 0) {
        throw std::runtime_error("crashdump fixture");
    }
    return 3;
}
