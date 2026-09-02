#pragma once
#include <QString>
#include <QVector>

// Stable catalog of every bindable application behavior.
// This is the single source of truth for action identity, scope, and label —
// the binding resolver and runtime dispatch both read from it
// instead of hard-coding button-to-behavior switches.
class ActionCatalog
{
public:
    // Context an action fires in. An action only dispatches while its scope
    // is the active one, so the same physical trigger can mean different
    // things in the overlay vs. the desktop gallery vs. clip playback.
    enum class Scope {
        Global,     // active regardless of what else is showing
        Desktop,    // desktop gallery has real Win32 foreground focus
        Overlay,    // in-game overlay is visible
        Playback    // a clip is focused/playing in the overlay or gallery lightbox
    };

    struct Action {
        QString id;             // stable identifier, e.g. "global.screenshot"
        Scope scope;
        QString label;          // short UI label, e.g. "Screenshot"
        QString description;    // one-line explanation for the binding editor
        bool bindable = true;   // false = fixed emergency route, never remappable
        // Fires again while the control stays held (d-pad steps, L1/R1 paging,
        // seeking). The conflict policy needs this: a button that repeats can
        // never be the first control of a chord, because holding it to reach
        // the second control would step the gallery on the way.
        bool repeats = false;
        // Reserved for actions whose button-down edge is part of their product
        // contract. Such a Press binding must never be offered for automatic
        // conversion to a release-based single tap.
        bool requiresImmediatePress = false;
    };

    // Full catalog, in a stable declaration order (used for default UI grouping).
    static const QVector<Action>& all();

    // Returns nullptr if id is unknown.
    static const Action* find(const QString& id);

    // Refreshes the representative cached label after a translator commit.
    // Bulk action-string migration remains a later localization item.
    static void retranslate();
};
