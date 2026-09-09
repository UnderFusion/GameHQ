#pragma once
#include <QHash>
#include <QString>
#include <QStringList>

// Load-status bookkeeping for the sound effects (docs/sound-system.md).
//
// Kept free of Qt Multimedia on purpose: QSoundEffect resolves its source
// asynchronously, may emit the same status twice, and needs a real audio
// device, none of which belongs in a decision this simple. SoundEngine maps
// QSoundEffect::Status onto Status and asks this class what to report, so the
// reporting rules stay deterministic and testable (tests/tst_soundengine.cpp).
class SoundLoadTracker
{
public:
    // Mirrors QSoundEffect::Status. Null means "no source resolved yet" and is
    // as final a failure as Error once the source has been set.
    enum class Status { Null, Loading, Ready, Error };

    // What the caller should write to the log for this transition.
    enum class Report {
        Nothing,   // still loading, or a status already reported for this event
        Ready,     // first time this effect became playable
        Failed,    // first time this effect resolved to an unusable state
    };

    // Records a status transition for one effect. Repeated or regressing
    // statuses for an already-resolved effect return Nothing, so a chatty
    // backend cannot produce duplicate log lines.
    Report note(const QString& event, Status status);

    // Non-empty exactly once, on the first failure of the first effect that
    // fails. Later failures are logged but never re-raised: one visible
    // warning per run is the whole point. The string lists the events that
    // had failed by that moment.
    QString takeFailureWarning();

    bool hasFailures() const { return !m_failed.isEmpty(); }
    QStringList failedEvents() const { return m_failed; }

private:
    QHash<QString, Report> m_resolved;   // event → Ready or Failed
    QStringList m_failed;                // failed events, in resolution order
    bool m_warningTaken = false;
};
