#pragma once

#include "input/ActionCatalog.h"
#include "input/BindingPattern.h"
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QString>
#include <QVector>

// Merges code-owned defaults with the sparse user overrides stored in SQLite.
// Runtime input and the binding editor both consume this same effective view.
class BindingResolver
{
public:
    struct Binding {
        QString deviceGroup;   // keyboard | controller | mouse
        QString deviceProfile; // empty = all devices; otherwise a fingerprint
        QString actionId;
        int slot = 1;
        QString triggerCode;
        QString activation = QStringLiteral("press");
        int holdMs = 0;
        bool unbound = false;
        // Taps a "tap" activation needs, 1-3. Meaningless for press and hold,
        // where the model pins it to 1.
        int tapCount = 1;

        // The gesture as the pattern model sees it. Rows that reach here have
        // already passed validation in reload(), so this never fails; a
        // hand-built Binding with a nonsense gesture degrades to plain press
        // rather than throwing at a call site that only wanted to compare two
        // bindings.
        // Inline on purpose: the conflict policy is a pure-logic unit and must
        // not have to link the resolver (and through it the database) just to
        // ask a binding what gesture it carries.
        GestureSpec gesture() const
        {
            const auto parsed = GestureSpec::parse(activation, tapCount, holdMs);
            return parsed.ok ? parsed.gesture : GestureSpec::press();
        }
        TriggerSpec trigger() const
        {
            const auto parsed = TriggerSpec::parse(triggerCode);
            return parsed.ok ? parsed.trigger : TriggerSpec::single(triggerCode);
        }
    };

    // The gesture a slot carries independently of what trigger sits in it.
    // Capturing into an empty slot and clearing a bound one both consult this,
    // so a slot whose meaning is "tap" stays a tap across clear and rebind.
    struct Gesture {
        QString activation = QStringLiteral("press");
        int holdMs = 0;
        int tapCount = 1;

        GestureSpec spec() const
        {
            const auto parsed = GestureSpec::parse(activation, tapCount, holdMs);
            return parsed.ok ? parsed.gesture : GestureSpec::press();
        }
    };

    explicit BindingResolver(CaptureDatabase* database);

    void setDefaultHoldMs(int milliseconds);
    // The duration a Hold binding with holdMs == 0 means. Built-in hold
    // defaults store 0 on purpose, so the configured value is the single
    // source of truth instead of being baked into every default row.
    int defaultHoldMs() const { return m_defaultHoldMs; }
    void reload();

    Gesture inheritedGesture(const QString& deviceGroup, const QString& deviceProfile,
                             const QString& actionId, int slot) const;

    // Legacy-profile aliasing: rows saved under `legacyProfile` (an
    // "xinput.slotN" fingerprint from before stable identity existed) keep
    // applying while `profile` is active, at lower precedence than rows saved
    // for `profile` itself. Existing overrides survive the upgrade unchanged;
    // they are never silently rewritten or broadened — promotion to the
    // stable identity is the user's explicit copy action in Settings.
    // Return true only when the stored alias view for `profile` actually
    // changed — repeated identical observations must be recognizable as no-ops.
    bool setProfileAlias(const QString& profile, const QString& legacyProfile);
    bool setProfileAliases(const QString& profile, const QStringList& legacyProfiles);

    // ---------------------------------------------------------------- cpo-p03
    // Migration bridge (docs/mapping-presets.md section 6). A chain may resolve
    // from its materialized preset only while that preset is proven equal to
    // the current ordered alias chain; every other chain keeps the legacy
    // merge above, byte-for-byte. Nothing here interprets assignment rows as a
    // winner rule - that stays cpo-p04.

    // The one canonical row validator: the same parse boundary reload() has
    // always applied, shared with the offline migration so v9 can never skip a
    // row differently than the runtime does.
    static bool validateOverrideRow(const BindingOverrideRow& row, QString* error);

    // Layer order for a chain: group-wide, then aliases in registration order,
    // then the exact profile - later wins. `profile` empty means the group-wide
    // chain itself (keyboard/mouse have no distinct identity layer).
    static QStringList chainLayers(const QString& profile, const QStringList& aliases);

    // The legacy merged table (defaults + layers, bindable rows only, unbound
    // rows suppress their default, only bound rows survive). effectiveBindings()
    // is this function; it is public so the migration can prove equality against
    // the exact code path input already exercises.
    static QVector<Binding> mergedTable(const QVector<Binding>& defaults,
                                        const QVector<Binding>& overrides,
                                        const QString& deviceGroup,
                                        const QStringList& layers);

    // The fold persisted as preset content: the last winning row per
    // (action, slot) across the chain's layers, unbound sentinels preserved so
    // the preset suppresses the same defaults the legacy table suppressed.
    // Rows that cannot be stored as a valid preset row are skipped (they stay
    // in binding_overrides and keep legacy resolution). `contributingLayers`,
    // when given, reports which layers supplied at least one storable row -
    // winners and shadowed rows alike, because both are part of the chain the
    // proof covers.
    static QVector<MappingPresetRow> foldChainRows(const QVector<Binding>& overrides,
                                                   const QString& deviceGroup,
                                                   const QString& profile,
                                                   const QStringList& aliases,
                                                   QStringList* contributingLayers = nullptr);

    // The table a stored preset's rows produce over the defaults - the "new"
    // side of the equivalence proof.
    static QVector<Binding> chainTableFromRows(const QVector<Binding>& defaults,
                                               const QString& deviceGroup,
                                               const QString& profile,
                                               const QVector<MappingPresetRow>& rows);

    // Content equality for the proof. Keyed by (action, slot): trigger, gesture
    // and unbound-state must match exactly; the profile label is storage detail
    // (legacy rows carry their original layer, preset rows the chain key).
    static bool chainTablesEqual(const QVector<Binding>& a, const QVector<Binding>& b);

    // Only the materializer may call this, and only after the equality proof
    // against the chain whose fingerprint it passes. The aliases are stored so
    // setProfileAliases() drops the view the moment the chain changes.
    bool activateMaterializedChain(const QString& deviceGroup, const QString& profile,
                                   const QString& presetId,
                                   const QVector<MappingPresetRow>& rows,
                                   const QStringList& aliases);
    void deactivateMaterializedChain(const QString& deviceGroup, const QString& profile);
    bool isMaterialized(const QString& deviceGroup, const QString& profile) const;
    QString materializedPreset(const QString& deviceGroup, const QString& profile) const;

    // Bridge inputs for the materializer; both are cheap copies of small sets.
    QStringList aliasesFor(const QString& profile) const { return m_profileAliases.value(profile); }
    QVector<Binding> validatedOverrides() const { return m_overrides; }
    // Bumped whenever the resolved facts change (reload, alias edit, chain
    // activation). Lets the materializer memoize without missing a change.
    quint64 revision() const { return m_revision; }

    QVector<Binding> effectiveBindings(const QString& deviceGroup,
                                       const QString& deviceProfile = {}) const;
    // Values inherited when the selected profile's own rows are absent.
    // Shared profiles inherit code defaults; controller-specific profiles also
    // inherit group-wide and legacy-alias rows.
    QVector<Binding> baselineBindings(const QString& deviceGroup,
                                      const QString& deviceProfile = {}) const;
    QVector<Binding> matching(const QString& deviceGroup,
                              const QString& deviceProfile,
                              const QString& triggerCode,
                              const GestureSpec& gesture,
                              ActionCatalog::Scope primaryScope,
                              ActionCatalog::Scope fallbackScope = ActionCatalog::Scope::Global) const;

    static QVector<Binding> defaultBindings();

private:
    // A chain whose preset content is proven equal to its legacy resolution.
    // The default resolution uses `table`; anything else goes through the
    // legacy merge. `aliases` is the chain fingerprint: when
    // setProfileAliases() sees a different ordered list (or reload() drops the
    // whole map because stored rows changed), the view is gone and the chain
    // resolves legacy until the materializer proves it again.
    struct MaterializedView {
        QString presetId;
        QStringList aliases;
        QVector<Binding> table;
    };
    static QString materializedKey(const QString& deviceGroup, const QString& profile);
    void rebuildGroupChains();

    CaptureDatabase* m_database = nullptr;
    QVector<Binding> m_overrides;
    QHash<QString, QStringList> m_profileAliases;
    QHash<QString, MaterializedView> m_materialized;
    quint64 m_revision = 1;
    int m_defaultHoldMs = 2000;
};
