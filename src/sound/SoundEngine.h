#pragma once
#include <QHash>
#include <QObject>
#include <QString>

class ConfigManager;
class QSoundEffect;

// Console-style UI sounds (docs/sound-system.md). Effects are pre-loaded
// from embedded resources at startup; play() respects "sounds.enabled" and
// "sounds.volume" from config live. Exposed to QML as "sounds".
class SoundEngine : public QObject
{
    Q_OBJECT
public:
    explicit SoundEngine(ConfigManager* config, QObject* parent = nullptr);

    // event: nav_tick | overlay_open | overlay_close | favorite | confirm |
    //        error | screenshot | replay_saved | capture_accepted
    Q_INVOKABLE void play(const QString& event);

private:
    ConfigManager* m_config;
    QHash<QString, QSoundEffect*> m_effects;
};
