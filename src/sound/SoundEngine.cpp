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

SoundLoadTracker::Status trackerStatus(QSoundEffect::Status status)
{
    switch (status) {
    case QSoundEffect::Loading:
        return SoundLoadTracker::Status::Loading;
    case QSoundEffect::Ready:
        return SoundLoadTracker::Status::Ready;
    case QSoundEffect::Error:
        return SoundLoadTracker::Status::Error;
    case QSoundEffect::Null:
        break;
    }
    return SoundLoadTracker::Status::Null;
}
}

SoundEngine::SoundEngine(ConfigManager* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    for (const char* event : kEvents) {
        const QString name = QLatin1String(event);
        auto* effect = new QSoundEffect(this);
        m_effects.insert(name, effect);
        // Connected before setSource(): a qrc source can resolve inside the
        // setter, and a connection made afterwards would miss that signal.
        connect(effect, &QSoundEffect::statusChanged, this,
                [this, name, effect] { handleStatus(name, effect); });
        effect->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(name)));
        // ...and read the status back, because a source that resolved inside
        // the setter emits nothing at all. Null here is not a verdict: it is
        // the pre-load value, and only counts as a failure once the backend
        // actually reports it through statusChanged.
        if (effect->status() != QSoundEffect::Null)
            handleStatus(name, effect);
    }
}

void SoundEngine::handleStatus(const QString& event, QSoundEffect* effect)
{
    const QSoundEffect::Status status = effect->status();
    switch (m_loadTracker.note(event, trackerStatus(status))) {
    case SoundLoadTracker::Report::Nothing:
        return;
    case SoundLoadTracker::Report::Ready:
        qInfo() << "Sounds: ready" << event << effect->source().toString();
        return;
    case SoundLoadTracker::Report::Failed:
        break;
    }

    qWarning() << "Sounds: failed to load" << event
               << effect->source().toString()
               << (status == QSoundEffect::Error ? "(decode or device error)"
                                                 : "(source did not resolve)");
    const QString summary = m_loadTracker.takeFailureWarning();
    if (summary.isEmpty())
        return;
    // Queued: an effect can resolve inside the constructor, before the owner
    // has had any chance to connect to loadFailed().
    QMetaObject::invokeMethod(
        this, [this, summary] { emit loadFailed(summary); }, Qt::QueuedConnection);
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
