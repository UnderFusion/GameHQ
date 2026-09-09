#include "sound/SoundEngine.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"

#include <QSoundEffect>
#include <QUrl>
#include <QDebug>

namespace
{
const char* kEvents[] = {
    "nav_tick", "overlay_open", "overlay_close", "favorite",
    "confirm", "error", "screenshot", "replay_saved",
    "capture_accepted",
};
}

SoundEngine::SoundEngine(ConfigManager* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    for (const char* event : kEvents) {
        auto* effect = new QSoundEffect(this);
        effect->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav")
                                   .arg(QLatin1String(event))));
        m_effects.insert(QLatin1String(event), effect);
    }
    qInfo() << "Sounds:" << m_effects.size() << "effects loaded";
}

void SoundEngine::play(const QString& event)
{
    if (!m_config->value(ConfigKeys::SoundsEnabled, true).toBool())
        return;
    QSoundEffect* effect = m_effects.value(event);
    if (!effect) {
        qWarning() << "Sounds: unknown event" << event;
        return;
    }
    const qreal volume = m_config->value(ConfigKeys::SoundsVolume, 80).toInt() / 100.0;
    effect->setVolume(qBound(0.0, volume, 1.0));
    effect->play();
}
