#pragma once

// Replay audio source selection (0.8.0 Beta 2).
//
// audio.source chooses what a replay clip records:
//   "game"    - only the captured game's process tree (Windows application
//               loopback), on whatever output device the game plays to;
//   "desktop" - everything on the default Windows output (compatibility);
//   "off"     - video only;
//   "auto"    - the default: game audio where Windows supports it, desktop
//               audio on older Windows. A saved legacy audio.enabled=false
//               still means off until the user picks a source explicitly.
//
// An explicit "game" never degrades to desktop audio: recording unrelated
// apps without telling the user is worse than failing with a reason.

#include <QString>

namespace AudioSourcePolicy {

enum class Source { Off, Game, Desktop };

struct Resolution {
    Source source = Source::Off;
    // Set when the user explicitly asked for game audio on a Windows build
    // that cannot capture a single process; the buffer must fail with it.
    bool gameAudioUnsupported = false;
};

inline constexpr unsigned long kMinimumProcessLoopbackBuild = 20348;

QString toConfigValue(Source source);
Resolution resolve(const QString& configured, bool legacyEnabled, bool gameAudioSupported);
// The concrete selection the Settings dropdown shows for a stored value.
QString effectiveSelection(const QString& configured, bool legacyEnabled,
                           bool gameAudioSupported);
bool isProcessLoopbackBuild(unsigned long windowsBuild);

} // namespace AudioSourcePolicy
