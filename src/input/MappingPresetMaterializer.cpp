#include "input/MappingPresetMaterializer.h"

#include <QDebug>

namespace {
const QString kController = QStringLiteral("controller");
const QString kPromoted = QStringLiteral("promoted");
const QString kUnverified = QStringLiteral("unverified");
const QString kRetired = QStringLiteral("retired");
} // namespace

MappingPresetMaterializer::MappingPresetMaterializer(CaptureDatabase* database,
                                                     BindingResolver* resolver)
    : m_database(database)
    , m_resolver(resolver)
{
}

void MappingPresetMaterializer::invalidate()
{
    m_memo.clear();
}

MappingPresetMaterializer::Outcome MappingPresetMaterializer::considerControllerChain(
    const QString& profile, bool durableIdentity)
{
    Outcome outcome;
    if (!m_database || !m_resolver || profile.isEmpty()) {
        outcome.verdict = Verdict::Inert;
        outcome.detail = QStringLiteral("no database, resolver or profile");
        return outcome;
    }

    const QStringList aliases = m_resolver->aliasesFor(profile);
    const auto memo = m_memo.constFind(profile);
    if (memo != m_memo.cend() && memo->revision == m_resolver->revision()
        && memo->aliases == aliases && memo->durable == durableIdentity)
        return memo->outcome;

    Outcome result = runControllerChain(profile, durableIdentity, aliases);
    // Stable outcomes are memoized: successes (unless bookkeeping still has to
    // retry), "nothing to do", and a weak-identity refusal (it cannot change
    // without a revision, alias or durability change - all part of the memo
    // key). A proof refusal or failure is re-attempted next time.
    if ((result.verdict == Verdict::Active && !result.retryNextTime)
        || result.verdict == Verdict::Inert
        || result.verdict == Verdict::NeedsDurable) {
        Memo entry;
        entry.revision = m_resolver->revision();
        entry.aliases = aliases;
        entry.durable = durableIdentity;
        entry.outcome = result;
        m_memo.insert(profile, entry);
    } else {
        m_memo.remove(profile);
    }
    return result;
}

MappingPresetMaterializer::Outcome MappingPresetMaterializer::runControllerChain(
    const QString& profile, bool durableIdentity, const QStringList& aliases)
{
    Outcome outcome;
    const QVector<BindingResolver::Binding> overrides = m_resolver->validatedOverrides();
    // The "before" side of the proof: exactly what legacy resolution produces
    // for this chain right now.
    const QVector<BindingResolver::Binding> legacy = BindingResolver::mergedTable(
        BindingResolver::defaultBindings(), overrides, kController,
        BindingResolver::chainLayers(profile, aliases));

    QStringList contributingLayers;
    const QVector<MappingPresetRow> fold = BindingResolver::foldChainRows(
        overrides, kController, profile, aliases, &contributingLayers);

    // Review correction A: a chain is materializable only while at least one
    // device-specific layer - an alias or the exact profile - contributes
    // storable behavior. Group-wide rows (layer "") alone are inherited
    // behavior, not an override, so such a pad must keep following the group
    // default instead of gaining a durable controller assignment the old model
    // never had.
    bool deviceSpecificRows = false;
    for (const QString& layer : contributingLayers) {
        if (!layer.isEmpty()) {
            deviceSpecificRows = true;
            break;
        }
    }
    if (!deviceSpecificRows) {
        outcome.verdict = Verdict::Inert;
        outcome.detail = QStringLiteral(
            "no device-specific stored rows; the pad keeps following the group default");
        return outcome;
    }

    MappingPresetSource own;
    for (const MappingPresetSource& source :
         m_database->listMappingPresetSources(kController)) {
        if (source.sourceKey == profile)
            own = source;
    }

    QString destId = own.convertedPresetId;
    QVector<MappingPresetRow> persisted;
    if (!destId.isEmpty())
        persisted = m_database->mappingPresetRows(destId);
    bool contentProven = !destId.isEmpty()
        && BindingResolver::chainTablesEqual(
               legacy, BindingResolver::chainTableFromRows(
                           BindingResolver::defaultBindings(), kController, profile, persisted));

    // No persistent write without a durable identity (section 6): a weak or
    // session-local pad keeps the compatibility chain, and this class creates
    // no assignment it cannot defend.
    if (!durableIdentity) {
        outcome.verdict = Verdict::NeedsDurable;
        outcome.presetId = destId;
        outcome.detail = contentProven
            ? QStringLiteral("content proven but identity is weak; no persistent write")
            : QStringLiteral("identity is weak; legacy resolution kept");
        return outcome;
    }

    if (!contentProven) {
        if (destId.isEmpty()) {
            // First materialization for this chain: one new migration preset
            // holding the fold, then verify the persisted candidate (section 6
            // review, correction E: content first, promotion later).
            const QString base = profile.size() > 32
                ? profile.left(29) + QStringLiteral("...")
                : profile;
            // Preset metadata, rows and the unverified source link land in ONE
            // transaction (review correction B): a crash or write failure
            // between them must not leave an orphan preset that the next retry
            // duplicates.
            destId = m_database->createUnverifiedMigrationSource(
                kController,
                m_database->uniqueMigrationPresetName(kController,
                                                      QStringLiteral("Migrated ") + base),
                fold, profile);
            if (destId.isEmpty()) {
                outcome.verdict = Verdict::Failed;
                outcome.detail = QStringLiteral(
                    "could not create the chain preset and its source atomically");
                return outcome;
            }
        } else {
            const MappingPreset preset = m_database->mappingPreset(destId);
            if (!canRewriteContent(preset, profile)) {
                outcome.verdict = Verdict::Refused;
                outcome.presetId = destId;
                outcome.detail = preset.id.isEmpty()
                    ? QStringLiteral("chain preset is missing")
                    : QStringLiteral("preset is not migration content or has foreign references");
                return outcome;
            }
            if (!m_database->replaceMappingPresetRows(destId, fold)) {
                outcome.verdict = Verdict::Failed;
                outcome.detail = QStringLiteral("content replacement failed");
                outcome.presetId = destId;
                return outcome;
            }
        }

        persisted = m_database->mappingPresetRows(destId);
        contentProven = BindingResolver::chainTablesEqual(
            legacy, BindingResolver::chainTableFromRows(
                        BindingResolver::defaultBindings(), kController, profile, persisted));
        if (!contentProven) {
            // The stored bytes cannot reproduce the live chain: never activate,
            // never promote. Legacy resolution keeps serving this pad.
            outcome.verdict = Verdict::Refused;
            outcome.presetId = destId;
            outcome.detail = QStringLiteral("persisted candidate is not equal to the legacy table");
            return outcome;
        }
    }

    // Promotion: storage writes the durable assignment and the promoted mark in
    // one transaction; the proof above is the only authorization it accepts.
    const MappingPresetSource current = m_database->mappingPresetSource(kController, profile);
    if (current.status != kPromoted) {
        if (current.sourceKey.isEmpty() || current.convertedPresetId != destId
            || current.status != kUnverified) {
            MappingPresetSource relink;
            relink.deviceGroup = kController;
            relink.sourceKey = profile;
            relink.convertedPresetId = destId;
            relink.status = kUnverified;
            if (!m_database->upsertMappingPresetSource(relink)) {
                outcome.verdict = Verdict::Failed;
                outcome.detail = QStringLiteral("could not relink the chain source");
                outcome.presetId = destId;
                return outcome;
            }
        }
        if (!m_database->promoteMappingPresetSource(kController, profile, profile)) {
            outcome.verdict = Verdict::Failed;
            outcome.detail = QStringLiteral("promotion transaction failed");
            outcome.presetId = destId;
            return outcome;
        }
    }

    // Classify every persisted source that contributed to this proven chain
    // (section 6 review, correction C): alias keys whose rows took part in the
    // chain - winners and shadowed rows alike - are consumed by this proof and
    // must not read as outstanding unknown controllers. They remain as
    // recovery records.
    bool sweepIncomplete = false;
    for (const QString& layer : contributingLayers) {
        if (layer.isEmpty() || layer == profile)
            continue;
        const MappingPresetSource aliasSource =
            m_database->mappingPresetSource(kController, layer);
        if (aliasSource.sourceKey.isEmpty() || aliasSource.status != kUnverified)
            continue;
        if (!m_database->setMappingPresetSourceStatus(kController, layer, kRetired)) {
            qWarning() << "Materializer: could not classify source" << layer
                       << "for the proven chain" << profile;
            sweepIncomplete = true;
        }
    }

    // The switch is the last step, and only now: the resolver serves the
    // stored content for exactly this ordered alias chain.
    if (!m_resolver->activateMaterializedChain(kController, profile, destId, persisted, aliases)) {
        outcome.verdict = Verdict::Failed;
        outcome.detail = QStringLiteral("activation refused");
        outcome.presetId = destId;
        return outcome;
    }

    outcome.verdict = Verdict::Active;
    outcome.presetId = destId;
    outcome.retryNextTime = sweepIncomplete;
    outcome.detail = sweepIncomplete
        ? QStringLiteral("chain materialized; source classification will retry")
        : QStringLiteral("chain materialized and proven");
    return outcome;
}

bool MappingPresetMaterializer::canRewriteContent(const MappingPreset& preset,
                                                  const QString& ownControllerKey) const
{
    // Migration content may only be replaced while it is still migration-owned:
    // a migration preset that nothing but this chain's own controller
    // assignment refers to. A foreign assignment (or any user preset) means the
    // content now belongs to somebody else - legacy keeps serving instead of
    // silently overwriting it (section 6 review, correction D seam).
    if (preset.id.isEmpty() || preset.origin != QLatin1String("migration"))
        return false;
    for (const MappingAssignment& assignment : m_database->listMappingAssignments(preset.deviceGroup)) {
        if (assignment.presetId != preset.id)
            continue;
        const bool ownChain = assignment.targetKind == QLatin1String("controller")
            && assignment.targetKey == ownControllerKey;
        if (!ownChain)
            return false;
    }
    return true;
}
