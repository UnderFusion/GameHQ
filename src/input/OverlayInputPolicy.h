#pragma once

#include "input/ActionCatalog.h"
#include "input/BindingPattern.h"

#include <QString>
#include <QStringList>
#include <QVector>

// GameHQ-side routing for the in-game overlay context (docs/overlay.md,
// "Input delivery"). Pure logic: no Win32, no window, no device, no database —
// the arbitration InputEngine and BindingResolver apply at runtime is decided
// here, so the contract can be audited without a game running.
//
// These rules only decide what GAMEHQ fires for a press. GameHQ reads the pad
// passively and never suppresses input to the game; what the game itself sees
// is decided by Windows and the game, not by this unit.
namespace OverlayInput {

// What GameHQ owns at the moment of the press. The OS foreground never enters
// this structure: overlay routing follows the overlay's own visibility, never
// whatever window happens to hold focus.
struct Context {
    bool overlayVisible = false;   // the in-game overlay is shown
    bool playbackActive = false;   // a focused clip is playing
    bool desktopFocused = false;   // the GameHQ desktop window holds foreground
};

// One binding row able to fire for the pressed trigger.
struct Candidate {
    QString actionId;
    ActionCatalog::Scope scope = ActionCatalog::Scope::Global;
    GestureSpec gesture = GestureSpec::press();
};

// Indices into the candidate list; the deliveries keep input order.
struct Selection {
    QVector<int> globalIndices;      // Global rows, minus declared substitutions
    QVector<int> contextualIndices;  // the winning context's rows (0 or 1 row)
    int deliveredCount() const { return globalIndices.size() + contextualIndices.size(); }
};

// The scope selection behind InputEngine::primaryScope/fallbackScope.
ActionCatalog::Scope primaryScope(const Context& context);
ActionCatalog::Scope fallbackScope(const Context& context);

// Resolve one press cycle: the rows whose gesture matches, split into the
// contextual winner and the Global rows that stay live, minus the pairs
// ContextOverrideCatalog declares as substitutions. This is the exact
// arbitration BindingResolver::matching() uses — that call site delegates here.
Selection select(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                 ActionCatalog::Scope primary, ActionCatalog::Scope fallback);

// select() with the scopes derived from the context — the view the contract
// tests and docs describe.
Selection select(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                 const Context& context);

// The action ids one press cycle delivers, in delivery order (globals first,
// then the contextual action) — what the audit asserts is never more than one
// for the shipped table.
QStringList deliveredActions(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                             const Context& context);

} // namespace OverlayInput
