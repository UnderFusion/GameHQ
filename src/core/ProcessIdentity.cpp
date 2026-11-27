#include "core/ProcessIdentity.h"

#include <QStringList>

#include <windows.h>

namespace ProcessIdentity
{
bool creationTimeOf(void* process, quint64& creationTime)
{
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exited, &kernel, &user))
        return false;
    creationTime = (quint64(creation.dwHighDateTime) << 32) | quint64(creation.dwLowDateTime);
    return true;
}
QString tokenFor(quint32 processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(processId));
    if (!process)
        return {};
    quint64 creationTime = 0;
    const bool read = creationTimeOf(process, creationTime);
    CloseHandle(process);
    if (!read)
        return {};
    return QStringLiteral("%1:%2").arg(processId).arg(creationTime);
}

QString currentToken()
{
    return tokenFor(quint32(GetCurrentProcessId()));
}

bool parseToken(const QString& token, quint32& processId, quint64& creationTime)
{
    const QStringList parts = token.trimmed().split(QLatin1Char(':'));
    if (parts.size() != 2)
        return false;
    bool idOk = false;
    bool timeOk = false;
    const quint32 id = parts.at(0).toUInt(&idOk);
    const quint64 created = parts.at(1).toULongLong(&timeOk);
    if (!idOk || !timeOk || id == 0)
        return false;
    processId = id;
    creationTime = created;
    return true;
}

WaitOutcome waitForExit(const QString& token, int timeoutMs)
{
    quint32 processId = 0;
    quint64 creationTime = 0;
    if (!parseToken(token, processId, creationTime))
        return WaitOutcome::Malformed;

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 DWORD(processId));
    if (!process) {
        // Only "there is no such process" proves it exited. Access denied and
        // everything else mean we cannot see it, and treating that as "gone"
        // is what let work start while the other process was still holding it.
        return GetLastError() == ERROR_INVALID_PARAMETER ? WaitOutcome::Exited
                                                         : WaitOutcome::Unverifiable;
    }

    quint64 actualCreationTime = 0;
    if (!creationTimeOf(process, actualCreationTime)) {
        CloseHandle(process);
        return WaitOutcome::Unverifiable;
    }
    if (actualCreationTime != creationTime) {
        // Same id, different process: Windows recycled it, so the one we were
        // asked about is already gone. Waiting here would block on a stranger.
        CloseHandle(process);
        return WaitOutcome::Exited;
    }

    const DWORD waited = WaitForSingleObject(process,
                                             timeoutMs < 0 ? INFINITE : DWORD(timeoutMs));
    CloseHandle(process);
    if (waited == WAIT_OBJECT_0)
        return WaitOutcome::Exited;
    return waited == WAIT_TIMEOUT ? WaitOutcome::StillRunning : WaitOutcome::Unverifiable;
}
}
