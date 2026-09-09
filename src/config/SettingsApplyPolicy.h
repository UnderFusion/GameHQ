#pragma once

#include <QString>
#include <QStringList>

// Decides whether a persisted setting can take effect in place or requires the
// replay pipeline to be rebuilt. Unknown forward-compatible keys are live by
// default; the unit-test completeness guard keeps every built-in key explicit.
class SettingsApplyPolicy
{
public:
    enum class Action {
        ApplyLive,
        RestartReplayBuffer,
    };

    static Action actionFor(const QString& key);
    static bool requiresReplayBufferRestart(const QString& key);
    static bool requiresReplayBufferRestart(const QStringList& keys);
    static QStringList classifiedKeys();
};
