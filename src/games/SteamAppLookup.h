#pragma once
#include <QString>

// Maps a game executable to the Steam app that installed it, using only the
// library's install manifests (`steamapps/appmanifest_<appid>.acf`). It never
// looks at Steam's controller configuration or any other Steam user data.
struct SteamApp
{
    QString appId;   // decimal Steam AppID, empty when the path is not a Steam install
    QString name;    // the manifest's store name, may be empty
    bool isValid() const { return !appId.isEmpty(); }
};

namespace SteamAppLookup
{
    // `executablePath` must sit inside `…/steamapps/common/<installdir>/…`;
    // anything else (Game Pass, Epic, a Steam shortcut to a non-Steam game)
    // returns an invalid SteamApp without touching the disk.
    SteamApp forExecutable(const QString& executablePath);
}
