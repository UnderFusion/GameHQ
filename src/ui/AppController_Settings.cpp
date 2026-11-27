// AppController: Settings-page configuration access and resets.
#include "ui/AppController.h"
#include "app/StartupManager.h"
#include "capture/AudioSourcePolicy.h"
#include "capture/CaptureUtil.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/SettingsApplyPolicy.h"
#include "config/SettingsCategories.h"
#include "localization/NativeText.h"
#include "sound/SoundEngine.h"
#include "sound/SoundLevels.h"
#include "storage/CaptureDatabase.h"
#include "ui/SettingsRouter.h"

#include <QDebug>
#include <QUrl>

namespace
{
bool parseCaptureKind(const QString& value, CaptureLocations::Kind& kind)
{
    if (value == QStringLiteral("screenshots")) {
        kind = CaptureLocations::Kind::Screenshots;
        return true;
    }
    if (value == QStringLiteral("clips")) {
        kind = CaptureLocations::Kind::Clips;
        return true;
    }
    return false;
}

}

QString AppController::setCaptureRoot(const QString& kindValue, const QUrl& folderUrl)
{
    CaptureLocations::Kind kind;
    if (!parseCaptureKind(kindValue, kind))
        return NativeText::get(
            //: Internal capture-location validation failure shown in Settings.
            //% "The capture type is invalid."
            QT_TRID_NOOP("gamehq.error.capture_location.type_invalid"),
            "The capture type is invalid.");
    if (!folderUrl.isLocalFile())
        return NativeText::get(
            //: Capture-location validation error shown in Settings.
            //% "Choose a local folder."
            QT_TRID_NOOP("gamehq.error.capture_location.local_folder_required"),
            "Choose a local folder.");

    QString error;
    if (!m_locations->setBaseRoot(kind, folderUrl.toLocalFile(), &error))
        return error;
    rescan();
    return {};
}

bool AppController::addCaptureExecutable(const QUrl& file)
{
    return file.isLocalFile() && m_config->setCaptureExecutableAllowed(file.toLocalFile(), true);
}

bool AppController::removeCaptureExecutable(const QString& path)
{
    return m_config->setCaptureExecutableAllowed(path, false);
}

QString AppController::resetCaptureRoot(const QString& kindValue)
{
    CaptureLocations::Kind kind;
    if (!parseCaptureKind(kindValue, kind))
        return NativeText::get(
            //% "The capture type is invalid."
            QT_TRID_NOOP("gamehq.error.capture_location.type_invalid"),
            "The capture type is invalid.");

    QString error;
    if (!m_locations->resetBaseRoot(kind, &error))
        return error;
    rescan();
    return {};
}

void AppController::removeWatchedFolder(const QString& path)
{
    m_db->removeWatchedFolder(path);
    emit foldersChanged();
}

QVariant AppController::config(const QString& key, const QVariant& fallback) const
{
    return m_config->value(key, fallback);
}

void AppController::setConfig(const QString& key, const QVariant& value)
{
    if (!routeConfigChange(key, value))
        return;
    m_config->setValue(key, value);
    m_config->save();   // a no-op when the value did not change
    if (SettingsApplyPolicy::requiresReplayBufferRestart(key))
        emit replaySettingsChanged();
}

void AppController::setConfigValues(const QVariantMap& values)
{
    bool hasPlainSetting = false;
    bool replayRestart = false;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!routeConfigChange(it.key(), it.value()))
            continue;
        hasPlainSetting = true;
        const bool changed = m_config->setValue(it.key(), it.value());
        if (changed && SettingsApplyPolicy::requiresReplayBufferRestart(it.key()))
            replayRestart = true;
    }
    if (!hasPlainSetting)
        return;
    m_config->save();
    if (replayRestart)
        emit replaySettingsChanged();
}

// Returns true when the caller should store the value in ConfigManager.
bool AppController::routeConfigChange(const QString& key, const QVariant& value)
{
    switch (m_settings->change(key, value)) {
    case SettingsRouter::Outcome::Rejected:
        emit configChanged(key, m_config->value(key, false));
        return false;
    case SettingsRouter::Outcome::Rescan:
        rescan();
        return false;
    case SettingsRouter::Outcome::Consumed:
        return false;
    case SettingsRouter::Outcome::Plain:
        break;
    }
    return true;
}

bool AppController::gameAudioSupported() const
{
    return AudioSourcePolicy::isProcessLoopbackBuild(CaptureUtil::windowsBuildNumber());
}

QString AppController::audioSourceSelection() const
{
    return AudioSourcePolicy::effectiveSelection(
        m_config->value(ConfigKeys::AudioSource, QStringLiteral("auto")).toString(),
        m_config->value(ConfigKeys::AudioEnabled, true).toBool(), gameAudioSupported());
}

QVariant AppController::configDefault(const QString& key, const QVariant& fallback) const
{
    return m_config->defaultValue(key, fallback);
}

bool AppController::configIsDefault(const QString& key) const
{
    return m_config->isDefault(key);
}

void AppController::resetConfig(const QString& key)
{
    switch (m_settings->reset(key)) {
    case SettingsRouter::Outcome::Rejected:
        emit configChanged(key, m_config->value(key, false));
        return;
    case SettingsRouter::Outcome::Rescan:
        rescan();
        return;
    case SettingsRouter::Outcome::Consumed:
        return;
    case SettingsRouter::Outcome::Plain:
        break;
    }
    if (!m_config->resetValue(key))
        return;
    m_config->save();
    if (SettingsApplyPolicy::requiresReplayBufferRestart(key))
        emit replaySettingsChanged();
}

void AppController::resetConfigGroup(const QString& prefix)
{
    const QStringList changedKeys = m_config->overriddenKeys(prefix);
    if (prefix == QStringLiteral("startup") || prefix.isEmpty())
        m_startup->setEnabled(false);
    if (prefix == QStringLiteral("storage") || prefix.isEmpty())
        m_locations->preserveCurrentRoots();
    if (!m_config->resetGroup(prefix))
        return;
    m_config->save();
    if (SettingsApplyPolicy::requiresReplayBufferRestart(changedKeys))
        emit replaySettingsChanged();
    if (prefix == QStringLiteral("storage") || prefix.isEmpty())
        rescan();
}

void AppController::resetAllConfig()
{
    const QStringList changedKeys = m_config->overriddenKeys();
    m_startup->setEnabled(false);
    m_locations->preserveCurrentRoots();
    if (!m_config->resetAll())
        return;
    m_config->save();
    if (SettingsApplyPolicy::requiresReplayBufferRestart(changedKeys))
        emit replaySettingsChanged();
    rescan();
}

// The preview deliberately goes through the ordinary play path: it obeys the
// UI-sounds master switch and the capture level exactly as a real capture
// does, so what the user hears here is what a capture will sound like.
void AppController::previewCaptureSound()
{
    if (!m_sounds) {
        qWarning() << "Sound preview requested before the sound engine exists";
        return;
    }
    m_sounds->play(SoundLevels::previewEvent());
}

void AppController::resetCategory(const QString& category)
{
    for (const QString& prefix : SettingsCategories::groups().value(category))
        resetConfigGroup(prefix);
    for (const QString& key : SettingsCategories::keys().value(category))
        resetConfig(key);
}

bool AppController::cursorCaptureControlSupported() const
{
    return CaptureUtil::windowsBuildNumber() >= 19041;
}
