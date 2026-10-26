#pragma once
#include <QString>
#include <QStringList>

#include <cstddef>
#include <windows.h>

// Local crash dumps. install() registers an unhandled-exception filter (and a
// std::terminate handler) that writes a small minidump into the logs folder:
// <logsDir>/gamehq-crash-YYYYMMDD-HHMMSS-<pid>.dmp. Dumps never leave the
// machine; the user attaches one to a bug report by hand if they choose to.
//
// Dumps are size-capped (stacks, thread info and the memory they reference,
// never full process memory) and retention-capped: at most kRetainedDumps
// files matching that exact name pattern are kept, oldest deleted first.
namespace CrashDump
{
    inline constexpr int kRetainedDumps = 5;

    // Call once the logs directory is known and owned by this process. Prunes
    // old dumps, loads dbghelp.dll from System32 and installs the handlers.
    // Everything the crash path needs is prepared here, so the filter itself
    // does no Qt or heap work. Installing again is a no-op.
    void install(const QString& logsDir);

    // Writes "gamehq-crash-YYYYMMDD-HHMMSS-<pid>.dmp" into out without any
    // allocation (crash-path safe). Returns false if capacity is too small.
    bool formatFileName(wchar_t* out, std::size_t capacity,
                        const SYSTEMTIME& time, DWORD processId);

    // True only for names formatFileName() produces; retention never touches
    // anything else in the logs folder.
    bool isDumpFileName(const QString& name);

    // The dump file names (of those given) to delete so that at most `keep`
    // remain, newest kept; returned newest first, so keep == 0 lists every
    // dump in age order. Names that are not dump names are ignored.
    QStringList namesToPrune(const QStringList& names, int keep);
}
