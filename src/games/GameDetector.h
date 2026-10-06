#pragma once
#include <QString>

namespace integration { class ExternalGameContext; }

// Identifies the current foreground window and decides whether it is a
// capturable "game" under the active capture.mode (docs/capture-engine.md).
// Fullscreen heuristic: the window rect covers its whole monitor. Windows
// shell surfaces (Explorer, Start, GameHQ itself…) are never treated as games.

struct ForegroundGame
{
    bool valid = false;          // a foreground window was found
    bool isGame = false;         // passes the "in a game" heuristic
    bool isFullscreen = false;   // window rect == monitor rect
    bool isExcludedProcess = false; // shell/system/GameHQ surfaces are never games
    bool hasExternalIdentity = false;
    bool viaShellFallback = false; // Xbox shell in front; target is the verified Playnite game
    QString externalSource;
    QString externalId;
    QString processName;         // e.g. "Cyberpunk2077.exe"
    QString executablePath;      // full foreground executable path, when Windows exposes it
    QString gameName;            // resolved display title (best of the sources below)
    QString windowTitle;         // raw window caption (GetWindowTextW)
    unsigned long pid = 0;       // foreground process id (for process-scoped audio)
    void* hwnd = nullptr;        // HWND (opaque here; cast in the .cpp)
    int x = 0, y = 0, w = 0, h = 0;   // window rect in screen space
};

namespace GameDetector
{
    ForegroundGame current();
    void setExternalContext(const integration::ExternalGameContext *context);

    // captureMode: only_in_games | whitelist | always (see ConfigManager).
    bool shouldCapture(const ForegroundGame& g, const QString& captureMode);
}
