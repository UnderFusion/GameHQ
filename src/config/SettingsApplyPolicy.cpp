#include "config/SettingsApplyPolicy.h"

#include "config/ConfigKeys.h"

#include <array>

namespace
{
using Key = QLatin1StringView;

constexpr std::array restartKeys{
    ConfigKeys::ReplayResolution,
    ConfigKeys::ReplayFps,
    ConfigKeys::ReplayBitrateMbps,
    ConfigKeys::ReplayLengthSeconds,
    ConfigKeys::ReplaySegmentSeconds,
    ConfigKeys::ReplayAuto,
    ConfigKeys::AudioEnabled,
};

constexpr std::array liveKeys{
    ConfigKeys::CaptureMode,
    ConfigKeys::CaptureHideBorder,
    ConfigKeys::CaptureScreenshotFormat,
    ConfigKeys::CaptureJpegQuality,
    ConfigKeys::CaptureScreenshotSound,
    ConfigKeys::CaptureScreenshotNotify,
    ConfigKeys::ReplayClipSound,
    ConfigKeys::ReplayClipNotify,
    ConfigKeys::ReplayManualIdleSeconds,
    ConfigKeys::InputShareHoldMs,
    ConfigKeys::InputDefaultHoldMs,
    ConfigKeys::InputMultiTapIntervalMs,
    ConfigKeys::InputChordWindowMs,
    ConfigKeys::InputModernControllerSupport,
    ConfigKeys::StorageScreenshotsRoot,
    ConfigKeys::StorageClipsRoot,
    ConfigKeys::StartupEnabled,
    ConfigKeys::StartupMinimized,
    ConfigKeys::SoundsEnabled,
    ConfigKeys::SoundsVolume,
    ConfigKeys::SoundsCaptureVolume,
    ConfigKeys::TrayCloseToTray,
    ConfigKeys::TrayMinimizeToTray,
    ConfigKeys::NotificationsEnabled,
    ConfigKeys::UiLanguage,
    ConfigKeys::UiPage,
    ConfigKeys::UiSettingsCategory,
    ConfigKeys::UiGalleryFilterCategory,
    ConfigKeys::UiGalleryFilterGame,
    ConfigKeys::UiWindowX,
    ConfigKeys::UiWindowY,
    ConfigKeys::UiWindowWidth,
    ConfigKeys::UiWindowHeight,
    ConfigKeys::ThemeActiveSkin,
    ConfigKeys::ThemeOverlayScrimStrength,
    ConfigKeys::UpdatesCheckAutomatically,
    ConfigKeys::UpdatesSkippedVersion,
};

template<std::size_t Size>
bool contains(const std::array<Key, Size>& keys, const QString& key)
{
    for (const Key candidate : keys) {
        if (key == candidate)
            return true;
    }
    return false;
}
}

SettingsApplyPolicy::Action SettingsApplyPolicy::actionFor(const QString& key)
{
    return contains(restartKeys, key) ? Action::RestartReplayBuffer : Action::ApplyLive;
}

bool SettingsApplyPolicy::requiresReplayBufferRestart(const QString& key)
{
    return actionFor(key) == Action::RestartReplayBuffer;
}

bool SettingsApplyPolicy::requiresReplayBufferRestart(const QStringList& keys)
{
    for (const QString& key : keys) {
        if (requiresReplayBufferRestart(key))
            return true;
    }
    return false;
}

QStringList SettingsApplyPolicy::classifiedKeys()
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(restartKeys.size() + liveKeys.size()));
    for (const Key key : restartKeys)
        result.append(QString(key));
    for (const Key key : liveKeys)
        result.append(QString(key));
    return result;
}
