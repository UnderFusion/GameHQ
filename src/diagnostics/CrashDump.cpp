#include "diagnostics/CrashDump.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <exception>
#include <string>

#include <dbghelp.h>

namespace
{
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

// Stacks, thread state and the memory those stacks point at: enough to read a
// crash, typically a few MiB at most. Never full process memory - captured
// frames and user data must not land in a file the user may share.
constexpr MINIDUMP_TYPE kDumpType = MINIDUMP_TYPE(
    MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);

// "gamehq-crash-" + 8 + "-" + 6 + "-" + up to 10 + ".dmp" + NUL.
constexpr std::size_t kMaxFileNameChars = 64;
// Logs folder path, '\\' and the file name. A longer directory skips dumps.
constexpr std::size_t kMaxPathChars = 1024;
// How long a crashing thread waits for the dump writer before the process
// continues dying anyway (a dump that deadlocks must not hang the crash).
constexpr DWORD kDumpTimeoutMs = 30000;
// Marks a dump written from std::terminate rather than a hardware exception.
constexpr DWORD kTerminateExceptionCode = 0xE0000001;

// Everything below is prepared by install() so the crash path only reads it.
wchar_t g_directory[kMaxPathChars] = {};
std::size_t g_directoryLength = 0;
MiniDumpWriteDumpFn g_writeDump = nullptr;
HANDLE g_requestEvent = nullptr;
HANDLE g_doneEvent = nullptr;
EXCEPTION_POINTERS* volatile g_exception = nullptr;
volatile DWORD g_crashThreadId = 0;
volatile LONG g_dumpStarted = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::terminate_handler g_previousTerminate = nullptr;
bool g_installed = false;

bool appendText(wchar_t* out, std::size_t capacity, std::size_t& used, const wchar_t* text)
{
    for (; *text; ++text) {
        if (used + 1 >= capacity)
            return false;
        out[used++] = *text;
    }
    out[used] = L'\0';
    return true;
}

bool appendNumber(wchar_t* out, std::size_t capacity, std::size_t& used,
                  unsigned long value, int minDigits)
{
    wchar_t digits[12];
    int count = 0;
    do {
        digits[count++] = wchar_t(L'0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 10);
    while (count < minDigits && count < 10)
        digits[count++] = L'0';
    if (used + std::size_t(count) + 1 > capacity)
        return false;
    while (count > 0)
        out[used++] = digits[--count];
    out[used] = L'\0';
    return true;
}

// Runs on the dump writer thread (normal stack, even after a stack overflow).
void writeDump(EXCEPTION_POINTERS* exception, DWORD threadId)
{
    if (!g_writeDump || g_directoryLength == 0)
        return;
    wchar_t path[kMaxPathChars + kMaxFileNameChars];
    std::size_t used = g_directoryLength;
    std::copy(g_directory, g_directory + g_directoryLength + 1, path);
    if (!appendText(path, sizeof(path) / sizeof(path[0]), used, L"\\"))
        return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (!CrashDump::formatFileName(path + used, sizeof(path) / sizeof(path[0]) - used,
                                   now, GetCurrentProcessId()))
        return;

    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = threadId;
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType,
                exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

DWORD WINAPI dumpWriterThread(LPVOID)
{
    WaitForSingleObject(g_requestEvent, INFINITE);
    writeDump(g_exception, g_crashThreadId);
    SetEvent(g_doneEvent);
    return 0;
}

// Only the first crash writes a dump; a second one (another thread, or the
// writer itself failing) just continues dying.
void requestDump(EXCEPTION_POINTERS* exception)
{
    if (InterlockedCompareExchange(&g_dumpStarted, 1, 0) != 0)
        return;
    g_exception = exception;
    g_crashThreadId = GetCurrentThreadId();
    SetEvent(g_requestEvent);
    WaitForSingleObject(g_doneEvent, kDumpTimeoutMs);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception)
{
    requestDump(exception);
    // The MinGW runtime registers its own filter before main(); keep its
    // behaviour (and Windows Error Reporting after it) unchanged.
    if (g_previousFilter)
        return g_previousFilter(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

// An uncaught C++ exception ends in std::terminate -> abort(), which never
// reaches the exception filter. Record the terminating thread's context so
// the dump still shows where it happened.
[[noreturn]] void terminateHandler()
{
    CONTEXT context;
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record = {};
    record.ExceptionCode = kTerminateExceptionCode;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64) || defined(__x86_64__)
    record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
#endif
    EXCEPTION_POINTERS pointers = {&record, &context};
    requestDump(&pointers);
    if (g_previousTerminate)
        g_previousTerminate();
    std::abort();
}

void pruneDumps(const QString& logsDir)
{
    QDir dir(logsDir);
    // Newest first. Room is left for this session's dump, so at most
    // kRetainedDumps files ever exist.
    const QStringList dumps = CrashDump::namesToPrune(dir.entryList(QDir::Files), 0);
    const int keep = CrashDump::kRetainedDumps - 1;
    for (const QString& name : dumps.mid(keep)) {
        if (!QFile::remove(dir.filePath(name)))
            qWarning().noquote() << "Crash dump: could not delete old" << name;
    }
    if (!dumps.isEmpty()) {
        qInfo().noquote() << "Crash dumps kept locally:" << qMin(dumps.size(), qsizetype(keep))
                          << "- newest" << dumps.first();
    }
}
} // namespace

namespace CrashDump
{
bool formatFileName(wchar_t* out, std::size_t capacity, const SYSTEMTIME& time,
                    DWORD processId)
{
    if (!out || capacity == 0)
        return false;
    std::size_t used = 0;
    out[0] = L'\0';
    return appendText(out, capacity, used, L"gamehq-crash-")
        && appendNumber(out, capacity, used, time.wYear, 4)
        && appendNumber(out, capacity, used, time.wMonth, 2)
        && appendNumber(out, capacity, used, time.wDay, 2)
        && appendText(out, capacity, used, L"-")
        && appendNumber(out, capacity, used, time.wHour, 2)
        && appendNumber(out, capacity, used, time.wMinute, 2)
        && appendNumber(out, capacity, used, time.wSecond, 2)
        && appendText(out, capacity, used, L"-")
        && appendNumber(out, capacity, used, processId, 1)
        && appendText(out, capacity, used, L".dmp");
}

bool isDumpFileName(const QString& name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^gamehq-crash-[0-9]{8}-[0-9]{6}-[0-9]{1,10}\\.dmp$"));
    return pattern.match(name).hasMatch();
}

QStringList namesToPrune(const QStringList& names, int keep)
{
    QStringList dumps;
    for (const QString& name : names) {
        if (isDumpFileName(name))
            dumps.append(name);
    }
    // Newest first: by timestamp, then by process id (several crashes in one
    // second). The process id has no fixed width, so compare it numerically.
    constexpr qsizetype kStampAt = 13;  // after "gamehq-crash-"
    constexpr qsizetype kStampLength = 15;
    std::sort(dumps.begin(), dumps.end(), [](const QString& a, const QString& b) {
        const int stamp = QStringView(a).mid(kStampAt, kStampLength)
                              .compare(QStringView(b).mid(kStampAt, kStampLength));
        if (stamp != 0)
            return stamp > 0;
        const qsizetype aPid = kStampAt + kStampLength + 1;
        return QStringView(a).mid(aPid).chopped(4).toULong()
            > QStringView(b).mid(aPid).chopped(4).toULong();
    });
    return keep >= 0 && dumps.size() > keep ? dumps.mid(keep) : QStringList{};
}

void install(const QString& logsDir)
{
    if (g_installed)
        return;
    g_installed = true;

    pruneDumps(logsDir);

    const std::wstring directory = QDir::toNativeSeparators(logsDir).toStdWString();
    if (directory.empty() || directory.size() >= kMaxPathChars - kMaxFileNameChars) {
        qWarning() << "Crash dump: logs folder path too long, dumps disabled";
        return;
    }
    std::copy(directory.begin(), directory.end(), g_directory);
    g_directory[directory.size()] = L'\0';
    g_directoryLength = directory.size();

    // System32 only: the process search path excludes the CWD and PATH, and
    // this makes the intent explicit (core/DllSearchPolicy.h).
    const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr,
                                           LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbghelp) {
        g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    }
    if (!g_writeDump) {
        qWarning() << "Crash dump: dbghelp.dll unavailable, dumps disabled";
        return;
    }

    // MiniDumpWriteDump runs on its own idle thread: a crashing thread may
    // have no stack left (stack overflow) and cannot safely start one then.
    g_requestEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE worker = (g_requestEvent && g_doneEvent)
        ? CreateThread(nullptr, 0, dumpWriterThread, nullptr, 0, nullptr)
        : nullptr;
    if (!worker) {
        qWarning() << "Crash dump: writer thread unavailable, dumps disabled";
        return;
    }
    CloseHandle(worker);

    g_previousFilter = SetUnhandledExceptionFilter(unhandledExceptionFilter);
    g_previousTerminate = std::set_terminate(terminateHandler);
}
} // namespace CrashDump
