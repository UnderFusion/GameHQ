#pragma once

#include <QString>

// Waiting for another process by process id alone is not safe on Windows: ids
// are reused, so by the time the waiter looks, the id can name a completely
// different program. Worse, OpenProcess failing does not mean the process is
// gone - it also fails when we are simply not allowed to see it.
//
// A token pairs the id with the process's creation time, which no recycled id
// can reproduce, and every answer below distinguishes "it exited" from "I could
// not tell".
namespace ProcessIdentity
{
enum class WaitOutcome {
    Exited,          // the named process is provably gone
    StillRunning,    // it was still alive when the timeout ran out
    Unverifiable,    // it could not be inspected - never assume it exited
    Malformed        // the token is not a token
};

// "<id>:<creation-time>" for this process, or empty if it cannot be read.
QString currentToken();
QString tokenFor(quint32 processId);
bool creationTimeOf(void* processHandle, quint64& creationTime);

bool parseToken(const QString& token, quint32& processId, quint64& creationTime);

// Waits for the process the token names. A negative timeout waits forever.
WaitOutcome waitForExit(const QString& token, int timeoutMs);
}
