#pragma once

#include "input/BindingResolver.h"

#include <QString>
#include <QStringList>

// Steam Input conflict help: pure logic, no I/O.
//
// Steam Input can translate a controller button for a Steam game (for example
// DualSense Create → Select) at the same time GameHQ reads it, so one press
// fires both the GameHQ shortcut and a game action. GameHQ never reads or
// changes Steam's controller configuration, so every verdict here is only a
// POSSIBLE conflict: "this button drives a GameHQ shortcut, and Steam Input
// may also send it to the game".
namespace SteamInputAdvice
{
    // The controls the helper knows how to advise on, in display order:
    // Create/Share (per-game Steam layout) and PS/Guide (Steam's global Guide
    // Button Chord configuration).
    QStringList advisedControls();

    // Which advised controls drive a Global-scope GameHQ action in this
    // effective controller table. Unbound rows and overlay/desktop-only actions
    // do not count: those never fire while the game is being played.
    QStringList boundControls(const QVector<BindingResolver::Binding>& table);

    // The per-game acknowledgement stored in config: the controls the user has
    // reviewed, or "*" when the user hid the help for that game entirely.
    inline constexpr const char* kDismissedAll = "*";
    QString serializeAck(const QStringList& controls);
    QStringList parseAck(const QString& stored);

    // Bound controls the user has not reviewed yet for this game. A newly bound
    // control (the GameHQ mapping changed) shows up again; "*" hides all.
    QStringList pendingControls(const QStringList& bound, const QString& storedAck);

    // Steam deep links. Both only open Steam's own UI; the user makes the change.
    QString gameLayoutUrl(const QString& appId);   // steam://controllerconfig/<appid>
    QString controllerSettingsUrl();               // steam://settings/controller
}
