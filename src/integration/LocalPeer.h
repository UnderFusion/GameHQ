#pragma once

#include <QString>

class QLocalSocket;

namespace integration
{
// Who is on the other end of an accepted named-pipe connection. Both local
// servers only admit the same Windows user, so this is not authentication
// (any same-user program can call itself anything); it makes every client
// attributable in the log and lets a handshake be compared with the program
// that is expected to send it.
struct LocalPeer
{
    quint32 processId = 0;
    QString executablePath;   // empty when the process could not be inspected
};

LocalPeer identifyLocalPeer(const QLocalSocket *socket);

// "<executable path> (pid N)" or "unknown process", for log lines.
QString describeLocalPeer(const LocalPeer &peer);
} // namespace integration
