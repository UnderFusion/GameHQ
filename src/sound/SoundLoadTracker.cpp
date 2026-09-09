#include "sound/SoundLoadTracker.h"

SoundLoadTracker::Report SoundLoadTracker::note(const QString& event, Status status)
{
    if (status == Status::Loading)
        return Report::Nothing;

    // Once an effect has resolved, it stays resolved. QSoundEffect can emit the
    // same status again (and can fall back to Null on teardown); neither is new
    // information for the user.
    if (m_resolved.contains(event))
        return Report::Nothing;

    if (status == Status::Ready) {
        m_resolved.insert(event, Report::Ready);
        return Report::Ready;
    }

    m_resolved.insert(event, Report::Failed);
    m_failed.append(event);
    return Report::Failed;
}

QString SoundLoadTracker::takeFailureWarning()
{
    if (m_warningTaken || m_failed.isEmpty())
        return {};
    m_warningTaken = true;
    return m_failed.join(QStringLiteral(", "));
}
