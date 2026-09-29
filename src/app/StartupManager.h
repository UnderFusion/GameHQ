#pragma once

#include <QString>

// Owns GameHQ's per-user Windows startup registration. The registry entry
// always targets the package launcher when one exists, so portable builds keep
// working after the real executable moved below app/.
//
// Installed, portable and development copies share the one `GameHQ` Run value
// but keep separate settings, so a copy only ever REMOVES the value when it
// points at that copy. Otherwise launching a copy with autostart off (a
// portable build, a dev build, a fresh package smoke run) silently deleted
// the installed copy's registration.
class StartupManager
{
public:
    // Explicit user choice: ON claims the Run value for this copy; OFF removes
    // it only when this copy owns it.
    bool setEnabled(bool enabled) const;

    // Launch-time reconcile with this copy's setting. Leaves the Run value
    // alone while it launches another copy that still exists, so starting a
    // portable or dev build never takes autostart over from the installed one.
    bool syncOnLaunch(bool enabled) const;

    // True when a Run command (quoted or bare, optional arguments) launches
    // `executable`. Compared case-insensitively on cleaned native paths.
    static bool commandTargets(const QString& command, const QString& executable);

private:
    static QString executablePath();
};
