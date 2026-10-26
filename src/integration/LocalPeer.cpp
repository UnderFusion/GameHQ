#include "integration/LocalPeer.h"

#include <QDir>
#include <QLocalSocket>

#include <iterator>

#include <windows.h>

namespace integration
{
LocalPeer identifyLocalPeer(const QLocalSocket *socket)
{
    LocalPeer peer;
    if (!socket)
        return peer;
    // On Windows the descriptor of a server-side QLocalSocket is the pipe
    // instance HANDLE.
    const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
    if (!pipe || pipe == INVALID_HANDLE_VALUE)
        return peer;
    ULONG processId = 0;
    if (!GetNamedPipeClientProcessId(pipe, &processId) || processId == 0)
        return peer;
    peer.processId = processId;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return peer;
    wchar_t buffer[MAX_PATH * 4];
    DWORD size = DWORD(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        peer.executablePath = QDir::fromNativeSeparators(QString::fromWCharArray(buffer, int(size)));
    CloseHandle(process);
    return peer;
}

QString describeLocalPeer(const LocalPeer &peer)
{
    if (peer.processId == 0)
        return QStringLiteral("unknown process");
    const QString path = peer.executablePath.isEmpty() ? QStringLiteral("<unreadable path>")
                                                       : peer.executablePath;
    return QStringLiteral("%1 (pid %2)").arg(path).arg(peer.processId);
}
} // namespace integration
