#pragma once

#include "storage/CaptureDatabase.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class CaptureDatabase;

// Winner rule for mapping presets (docs/mapping-presets.md sections 4 and 7,
// implemented by cpo-p04).
//
// resolve() answers one question with exactly one winner and one stated source:
// "given this device group, this controller identity chain and this running
// game, which preset serves, and why?"
//
// Precedence, highest first - the first usable hit wins:
//   1. game           assignment(group, "game", GameIdentity::executableKey(X))
//   2. controller     the caller's TYPED chain, in the given order; each
//                     candidate is queried ONLY for its declared kind.
//                     Controller group only; keyboard and mouse skip this step
//                     entirely.
//   3. group_default  assignment(group, "group_default", "")
//   4. builtin        nothing matched; presetId is empty
//
// A hit whose preset row is missing or belongs to another device group is
// skipped, reported once through staleReports() and the chain continues. An
// EMPTY preset is a valid winner: that is the supported "built-in defaults for
// this game" opt-out, never a fall-through.
//
// Boundaries:
// - Decision only. Applying a winner to the live table, switching mid-press and
//   gesture invalidation is cpo-p05; the cpo-p03 migration bridge keeps serving
//   until that wiring lands, and nothing here bypasses its proof state.
// - Identity is never inferred here. The caller owns provenance: it presents
//   IdentityCandidate entries in precedence order (the exact proven identity
//   first, then its compatibility aliases), and the resolver matches each key
//   exactly against its declared kind only - so a key from another provider can
//   never attract an assignment by guessing, and a forged or stale row in the
//   other kind's namespace cannot shadow a legitimate hit.
// - Kinds never conflate: a game key can only match a "game" row, a slot
//   fingerprint only a "legacy_slot" row, a durable pad key only a "controller"
//   row, and a group default never carries a key. No syntax test inside this
//   resolver decides durability - only the caller's declared kind does.
class MappingAssignmentResolver
{
public:
    // One step of the caller's controller identity chain, with the provenance
    // the caller can actually prove:
    //   - "controller"  - a durable physical-controller identity (a strong
    //                     generation-free logicalId). Only ever supplied for an
    //                     identity the runtime chain has proven durable.
    //   - "legacy_slot" - a slot/provider compatibility identity (xinput.slotN,
    //                     winmm.slotN, an unresolved provider key).
    // Any other kind is ignored: the typed chain carries controller targets
    // only. A weak or session-local identity is never presented as a persisted
    // "controller" candidate merely because its text looks durable.
    struct IdentityCandidate
    {
        QString targetKind;
        QString targetKey;
    };

    struct Resolution
    {
        QString presetId;    // empty => built-in defaults
        QString source;      // "game" | "controller" | "group_default" | "builtin"
        QString targetKind;  // winning assignment kind; empty for builtin
        QString targetKey;   // winning key; empty for group_default/builtin
        // Rules skipped because their preset was unusable, e.g.
        // "game:<key> -> missing preset <id>" (diagnostics and Settings copy).
        QStringList steppedDown;
    };

    struct StaleAssignment
    {
        QString deviceGroup;
        QString targetKind;
        QString targetKey;
        QString presetId;
        QString reason;      // "missing_preset" | "wrong_group"
    };

    explicit MappingAssignmentResolver(CaptureDatabase* database);

    // `identityChain` is the caller's typed chain in precedence order (exact
    // proven identity first, then aliases); every candidate is matched against
    // its declared kind only, and the whole chain is ignored for the keyboard
    // and mouse groups. `gameExecutableKey` is the foreground executable path
    // or key; it is canonicalized with canonicalGameKey() before matching.
    Resolution resolve(const QString& deviceGroup, const QVector<IdentityCandidate>& identityChain,
                       const QString& gameExecutableKey);

    // Canonical game key for assignment matching. Delegates to
    // GameIdentity::executableKey(), the single normalization shared with the
    // storage write path: idempotent, lowercased; the canonical file path while
    // the executable exists, a cleaned path otherwise. Both the stored key and
    // the live key go through it, so matching never depends on the casing or
    // separators a caller happens to use.
    static QString canonicalGameKey(const QString& executablePathOrKey);

    // Deduplicated by (deviceGroup, targetKind, targetKey, presetId): one broken
    // assignment is reported once, not on every resolution.
    QVector<StaleAssignment> staleReports() const;
    void clearStaleReports();

private:
    // Returns the winner's preset id when the assignment's preset is usable and
    // appends a steppedDown note plus a deduplicated stale report when it is
    // not. An assignment row without a preset id means "no hit".
    QString usablePresetId(const QString& deviceGroup, const MappingAssignment& hit,
                           Resolution& resolution);

    CaptureDatabase* m_database = nullptr;
    QVector<StaleAssignment> m_stale;
    QSet<QString> m_staleSeen;
};
