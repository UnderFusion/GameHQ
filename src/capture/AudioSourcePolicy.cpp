#include "capture/AudioSourcePolicy.h"

namespace AudioSourcePolicy {

QString toConfigValue(Source source)
{
    switch (source) {
    case Source::Game:
        return QStringLiteral("game");
    case Source::Desktop:
        return QStringLiteral("desktop");
    case Source::Off:
        break;
    }
    return QStringLiteral("off");
}

Resolution resolve(const QString& configured, bool legacyEnabled, bool gameAudioSupported)
{
    Resolution out;
    if (configured == QLatin1String("off"))
        return out;
    if (configured == QLatin1String("desktop")) {
        out.source = Source::Desktop;
        return out;
    }
    if (configured == QLatin1String("game")) {
        out.source = Source::Game;
        out.gameAudioUnsupported = !gameAudioSupported;
        return out;
    }
    // "auto" or anything unknown: keep an old explicit opt-out, otherwise
    // pick the best source this Windows build offers.
    if (!legacyEnabled)
        return out;
    out.source = gameAudioSupported ? Source::Game : Source::Desktop;
    return out;
}

QString effectiveSelection(const QString& configured, bool legacyEnabled,
                           bool gameAudioSupported)
{
    return toConfigValue(resolve(configured, legacyEnabled, gameAudioSupported).source);
}

bool isProcessLoopbackBuild(unsigned long windowsBuild)
{
    return windowsBuild >= kMinimumProcessLoopbackBuild;
}

} // namespace AudioSourcePolicy
