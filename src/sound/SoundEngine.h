#pragma once
#include <QHash>
#include <QObject>
#include <QString>

#include "sound/SoundLoadTracker.h"

class ConfigManager;
class QSoundEffect;

// Console-style UI sounds (docs/sound-system.md). Effects are pre-loaded
// from embedded resources at startup; play() respects "sounds.enabled" and
// "sounds.volume" from config live. Exposed to QML as "sounds".
//
// Loading is asynchronous: each effect reports Ready or a real error through
// QSoundEffect::statusChanged, which is logged per effect. The first effect
// that fails also raises loadFailed() once, so the app can show a single
// visible warning instead of silently playing nothing.
class SoundEngine : public QObject
{
    Q_OBJECT
public:
    explicit SoundEngine(ConfigManager* config, QObject* parent = nullptr);

    // event: nav_tick | overlay_open | overlay_close | favorite | confirm |
    //        error | screenshot | replay_saved | capture_accepted
    Q_INVOKABLE void play(const QString& event);

signals:
    // Emitted at most once per run, on the first effect that fails to load.
    // failedEvents is a comma-separated list of the events that had failed by
    // that moment — log context, not user-facing text.
    void loadFailed(const QString& failedEvents);

private:
    void handleStatus(const QString& event, QSoundEffect* effect);

    ConfigManager* m_config;
    QHash<QString, QSoundEffect*> m_effects;
    SoundLoadTracker m_loadTracker;
};
