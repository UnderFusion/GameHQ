#pragma once

#include "input/BindingResolver.h"
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// Runtime compatibility/materialization for the mapping migration
// (docs/mapping-presets.md section 6, owned by cpo-p03).
//
// When a live controller attaches with a durable identity, this class folds the
// chain `group-wide < ordered aliases < exact profile` into the chain's
// migration preset, proves the persisted content reproduces the legacy
// effective table, and only then lets storage atomically write the durable
// assignment (promoteMappingPresetSource). Until a chain passes that proof it
// keeps legacy resolution.
//
// Boundaries (reviewer corrections B/D):
// - the bridge is driven by migration proof state, never by assignment rows:
//   no game targets, no controller-vs-slot-vs-default winner rule (cpo-p04);
// - a weak/session-local identity never causes a persistent write;
// - content may only be rewritten while the preset is a migration preset with
//   no foreign references, so a later user edit (cpo-p06) can never be
//   silently overwritten by legacy materialization;
// - the resolver drops a materialized view the moment its alias chain changes,
//   so no proof is ever executed against a different chain.
class MappingPresetMaterializer
{
public:
    enum class Verdict {
        Active,        // chain proven; the resolver now serves the preset table
        Inert,         // nothing stored to migrate for this chain
        NeedsDurable,  // weak identity: legacy stays, no persistent write
        Refused,       // proof or storage rules refused (see `detail`)
        Failed         // a real write/transaction failure; legacy stays intact
    };
    struct Outcome {
        Verdict verdict = Verdict::Inert;
        QString presetId;
        QString detail;
        // Bookkeeping only: a successful chain whose source sweep still has
        // work to retry does not short-circuit the next attach.
        bool retryNextTime = false;
    };

    MappingPresetMaterializer(CaptureDatabase* database, BindingResolver* resolver);

    // Called after setProfileAliases() installed the chain (`profile` is the
    // exact key; the resolver holds the ordered aliases). O(1) on a memo hit;
    // the full path runs on attach, rekey and after any reload().
    Outcome considerControllerChain(const QString& profile, bool durableIdentity);

    // Drops memoized verdicts (tests; after external database repair).
    void invalidate();

private:
    struct Memo {
        quint64 revision = 0;
        QStringList aliases;
        bool durable = false;
        Outcome outcome;
    };
    Outcome runControllerChain(const QString& profile, bool durableIdentity,
                               const QStringList& aliases);
    bool canRewriteContent(const MappingPreset& preset, const QString& ownControllerKey) const;

    CaptureDatabase* m_database = nullptr;
    BindingResolver* m_resolver = nullptr;
    QHash<QString, Memo> m_memo;
};
