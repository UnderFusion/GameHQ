// Runtime materialization of the mapping migration (docs/mapping-presets.md
// section 6, cpo-p03). These tests exercise the bounded compatibility phase on
// the exact boundary the reviewer demanded:
//
// - a legacy v8 database is converted by applyV9, then a LIVE chain is
//   installed on the resolver (the InputEngine wiring does the same after
//   setProfileAliases) and the materializer has to prove `legacy effective
//   table == persisted preset table` before anything switches;
// - weak identities never write, foreign references are never overwritten,
//   alias-chain changes invalidate the proof, and every failure path leaves
//   legacy resolution intact;
// - the equivalence is asserted against BindingResolver::mergedTable, i.e. the
//   same canonical composition the runtime serves from - never a test-local
//   reimplementation.

#include "input/BindingResolver.h"
#include "input/MappingPresetMaterializer.h"
#include "storage/CaptureDatabase.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

const QString kController = QStringLiteral("controller");
const QString kKeyboard = QStringLiteral("keyboard");

// One stored legacy row (device_group is controller except for the keyboard
// group test). Mirrors the v4+ column list of binding_overrides.
struct LegacyRow {
    const char* profile;
    const char* action;
    int slot;
    const char* trigger;
    const char* activation;
    int holdMs;
    int unbound;
    int tapCount;
};

LegacyRow row(const char* profile, const char* action, int slot, const char* trigger,
              const char* activation = "press", int holdMs = 0, int unbound = 0,
              int tapCount = 1)
{
    return {profile, action, slot, trigger, activation, holdMs, unbound, tapCount};
}

LegacyRow keyRow(const char* profile, const char* action, int slot, const char* trigger,
                 const char* activation = "press", int holdMs = 0, int unbound = 0,
                 int tapCount = 1)
{
    // Same as row(); separate name keeps the keyboard fixture readable.
    return {profile, action, slot, trigger, activation, holdMs, unbound, tapCount};
}

// Builds a v8-shaped database: current schema, v9 tables empty, legacy rows
// inserted raw, `user_version = 8` - so CaptureDatabase::open() runs applyV9
// for real, exactly like an existing installation upgrading.
bool buildLegacyFixture(const QString& path, const QVector<LegacyRow>& rows,
                        const QString& group = kController)
{
    {
        CaptureDatabase created(path);
        if (!created.open())
            return false;
    }
    QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));

    bool ok = true;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("materializerfixture"));
        raw.setDatabaseName(path);
        ok = raw.open();
        if (ok) {
            QSqlQuery(QStringLiteral("PRAGMA foreign_keys = ON"), raw);
            QSqlQuery q(raw);
            const QStringList cleanup = {
                QStringLiteral("DELETE FROM mapping_assignments"),
                QStringLiteral("DELETE FROM mapping_preset_sources"),
                QStringLiteral("DELETE FROM mapping_preset_rows"),
                QStringLiteral("DELETE FROM mapping_presets"),
                QStringLiteral("DELETE FROM binding_overrides"),
            };
            for (const QString& statement : cleanup) {
                if (!q.exec(statement))
                    ok = false;
            }
            for (const LegacyRow& entry : rows) {
                QSqlQuery insert(raw);
                insert.prepare(QStringLiteral(
                    "INSERT INTO binding_overrides "
                    "(device_group, device_profile, action_id, slot, trigger_code, "
                    " activation, hold_ms, unbound, tap_count) "
                    "VALUES (:group, :profile, :action, :slot, :trigger, :activation, "
                    ":hold, :unbound, :taps)"));
                insert.bindValue(QStringLiteral(":group"), group);
                insert.bindValue(QStringLiteral(":profile"), QString::fromLatin1(entry.profile));
                insert.bindValue(QStringLiteral(":action"), QString::fromLatin1(entry.action));
                insert.bindValue(QStringLiteral(":slot"), entry.slot);
                insert.bindValue(QStringLiteral(":trigger"), QString::fromLatin1(entry.trigger));
                insert.bindValue(QStringLiteral(":activation"),
                                 QString::fromLatin1(entry.activation));
                insert.bindValue(QStringLiteral(":hold"),
                                 entry.holdMs > 0 ? QVariant(entry.holdMs) : QVariant());
                insert.bindValue(QStringLiteral(":unbound"), entry.unbound);
                insert.bindValue(QStringLiteral(":taps"), entry.tapCount);
                if (!insert.exec())
                    ok = false;
            }
            if (ok && !q.exec(QStringLiteral("PRAGMA user_version = 8")))
                ok = false;
        }
        raw.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("materializerfixture"));
    return ok;
}

QString triggerFor(const BindingResolver& resolver, const QString& group, const QString& profile,
                   const QString& action, int slot)
{
    for (const BindingResolver::Binding& binding : resolver.effectiveBindings(group, profile)) {
        if (binding.actionId == action && binding.slot == slot)
            return binding.triggerCode;
    }
    return QString();
}

int countAssignments(CaptureDatabase& db, const QString& kind)
{
    int count = 0;
    for (const MappingAssignment& assignment : db.listMappingAssignments(kController)) {
        if (assignment.targetKind == kind)
            ++count;
    }
    return count;
}

} // namespace

class PresetMaterializerTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void provesAndPromotesAnExactChain();
    void foldsTwoAliasesAndTheExactKeyIntoOneChain();
    void weakIdentityWritesNothingAndKeepsLegacy();
    void inertChainDoesNothing();
    void aliasChainChangeInvalidatesTheProof();
    void repeatedRunsAreIdempotent();
    void promotionFailureLeavesNoPartialStateAndRetries();
    void classificationRetriesAfterAFailedSweep();
    void slotAliasRowsFlowIntoTheChainButStaySlotScoped();
    void foreignReferenceRefusesContentRewrite();
    void keyboardGroupDefaultActivatesOnlyAfterProof();
    void controllerGroupDefaultDoesNotActivateChains();
    // Review correction A: a pad with no stored rows of its own must not gain a
    // per-device materialization just because its group has group-wide rows.
    void groupWideOnlyPadStaysOnLegacyResolution();
    // Review correction B: preset + source link are one transaction, so a
    // failed source write leaves no orphan preset and the retry is single.
    void sourceWriteFailureLeavesNoOrphanPresetAndRetries();

private:
    bool openApp();
    void closeApp();
    QSqlQuery rawExec(const QString& sql, bool* ok = nullptr);

    QTemporaryDir m_dir;
    QString m_path;
    std::unique_ptr<CaptureDatabase> m_db;
    std::unique_ptr<BindingResolver> m_resolver;
    std::unique_ptr<MappingPresetMaterializer> m_materializer;
};

void PresetMaterializerTest::init()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("gamehq.db"));
}

void PresetMaterializerTest::cleanup()
{
    closeApp();
}

bool PresetMaterializerTest::openApp()
{
    m_db = std::make_unique<CaptureDatabase>(m_path);
    if (!m_db->open())
        return false;
    m_resolver = std::make_unique<BindingResolver>(m_db.get());
    m_materializer = std::make_unique<MappingPresetMaterializer>(m_db.get(), m_resolver.get());
    m_resolver->reload();
    return true;
}

void PresetMaterializerTest::closeApp()
{
    m_materializer.reset();
    m_resolver.reset();
    m_db.reset();
    QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
}

QSqlQuery PresetMaterializerTest::rawExec(const QString& sql, bool* ok)
{
    QSqlQuery result;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("materializerraw"));
        raw.setDatabaseName(m_path);
        if (!raw.open()) {
            if (ok)
                *ok = false;
        } else {
            QSqlQuery query(raw);
            const bool executed = query.exec(sql);
            if (ok)
                *ok = executed;
        }
        raw.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("materializerraw"));
    return result;
}

// A durable pad whose exact key was already converted offline: content is
// `group-wide + own rows`, the chain has no aliases, so the persisted bytes
// must prove equal and promotion may follow immediately.
void PresetMaterializerTest::provesAndPromotesAnExactChain()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.toggle_overlay", 1, "gamepad.guide"),
        row("controller-aaaa", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());
    QCOMPARE(m_db->schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);

    const QString profile = QStringLiteral("controller-aaaa");
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    QVERIFY(!legacy.isEmpty());

    m_resolver->setProfileAliases(profile, {});
    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QVERIFY(!outcome.presetId.isEmpty());

    // Durable assignment + promoted source, both written by storage.
    const MappingAssignment assignment =
        m_db->mappingAssignment(kController, QStringLiteral("controller"), profile);
    QCOMPARE(assignment.presetId, outcome.presetId);
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("promoted"));

    // Resolution switched, and the table is byte-identical to legacy.
    QVERIFY(m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    QCOMPARE(triggerFor(*m_resolver, kController, profile, QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.capture"));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.toggle_overlay"), 1),
             QStringLiteral("gamepad.guide"));
}

// Reviewer correction C: two persisted alias sources plus one exact source,
// all conflicting on the same (action, slot). The verified chain owns one
// canonical preset; aliases are consumed, not left looking like unresolved
// controllers.
void PresetMaterializerTest::foldsTwoAliasesAndTheExactKeyIntoOneChain()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("xinput.slot1", "global.screenshot", 1, "gamepad.capture", "tap"),
        row("controller-bbbb", "global.screenshot", 1, "gamepad.menu", "hold", 1500),
        row("controller-cccc", "global.screenshot", 1, "gamepad.view"),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-cccc");
    m_resolver->setProfileAliases(
        profile, {QStringLiteral("xinput.slot1"), QStringLiteral("controller-bbbb")});
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.view"));

    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));

    // The exact key owns the assignment; the slice key keeps its slot-scoped
    // target; the opaque alias is consumed as a retired recovery record.
    QCOMPARE(m_db->mappingAssignment(kController, QStringLiteral("controller"), profile).presetId,
             outcome.presetId);
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("promoted"));
    QCOMPARE(m_db->mappingPresetSource(kController, QStringLiteral("controller-bbbb")).status,
             QStringLiteral("retired"));
    QVERIFY(m_db->mappingPresetSource(kController, QStringLiteral("xinput.slot1")).sourceKey.isEmpty());
    QVERIFY(!m_db->mappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot1"))
                 .presetId.isEmpty());

    // Folding must not change a single visible mapping.
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.view"));

    // The chain content itself carries the winner (recovery evidence).
    int winners = 0;
    for (const MappingPresetRow& content : m_db->mappingPresetRows(outcome.presetId)) {
        if (content.actionId == QLatin1String("global.screenshot") && content.slot == 1) {
            QCOMPARE(content.triggerCode, QStringLiteral("gamepad.view"));
            ++winners;
        }
    }
    QCOMPARE(winners, 1);
}

void PresetMaterializerTest::weakIdentityWritesNothingAndKeepsLegacy()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.toggle_overlay", 1, "gamepad.guide"),
        row("controller-aaaa", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-aaaa");
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    const int presetsBefore = m_db->listMappingPresets().size();
    const int sourcesBefore = m_db->listMappingPresetSources().size();

    m_resolver->setProfileAliases(profile, {});
    const auto outcome = m_materializer->considerControllerChain(profile, false);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::NeedsDurable));

    // No persistent write of any kind: no promotion, no assignment, no preset.
    QCOMPARE(m_db->listMappingPresets().size(), presetsBefore);
    QCOMPARE(m_db->listMappingPresetSources().size(), sourcesBefore);
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("unverified"));
    QVERIFY(m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                .presetId.isEmpty());
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
}

void PresetMaterializerTest::inertChainDoesNothing()
{
    QVERIFY(buildLegacyFixture(m_path, {}));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-dddd");
    m_resolver->setProfileAliases(profile, {});
    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Inert));
    QCOMPARE(m_db->listMappingPresets().size(), 0);
    QCOMPARE(m_db->listMappingAssignments(kController).size(), 0);
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
}

// Reviewer correction D: a proof is valid only for the chain it was computed
// against. When providers attach (or the pad rekeys) the alias list grows; the
// bridge must drop, re-prove and only then serve again.
void PresetMaterializerTest::aliasChainChangeInvalidatesTheProof()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("xinput.slot1", "global.screenshot", 2, "gamepad.capture", "tap"),
        row("controller-cccc", "global.screenshot", 1, "gamepad.menu", "hold", 1500),
        row("controller-bbbb", "global.screenshot", 2, "gamepad.view"),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-cccc");
    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("xinput.slot1")}));
    auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 2),
             QStringLiteral("gamepad.capture"));

    // The chain grows: a new alias with a winning row appears.
    QVERIFY(m_resolver->setProfileAliases(
        profile, {QStringLiteral("xinput.slot1"), QStringLiteral("controller-bbbb")}));
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    // Until re-proof, the legacy table serves - which is the old behavior, so a
    // larger alias chain can never execute against the stale proof.
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 2),
             QStringLiteral("gamepad.view"));

    outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QVERIFY(m_resolver->isMaterialized(kController, profile));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 2),
             QStringLiteral("gamepad.view"));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.menu"));
    // Exactly one durable assignment for the chain - rematerialization updated
    // content, it did not duplicate targets.
    QCOMPARE(countAssignments(*m_db, QStringLiteral("controller")), 1);
}

void PresetMaterializerTest::repeatedRunsAreIdempotent()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.toggle_overlay", 1, "gamepad.guide"),
        row("controller-aaaa", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-aaaa");
    m_resolver->setProfileAliases(profile, {});
    const auto first = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(first.verdict), int(MappingPresetMaterializer::Verdict::Active));
    const int presetsAfterFirst = m_db->listMappingPresets().size();
    const int assignmentsAfterFirst = m_db->listMappingAssignments(kController).size();

    const auto second = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(second.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QCOMPARE(second.presetId, first.presetId);
    QCOMPARE(m_db->listMappingPresets().size(), presetsAfterFirst);
    QCOMPARE(m_db->listMappingAssignments(kController).size(), assignmentsAfterFirst);

    // Even with the memo dropped, the stored state is already sufficient.
    m_materializer->invalidate();
    const auto third = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(third.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QCOMPARE(third.presetId, first.presetId);
    QCOMPARE(m_db->listMappingPresets().size(), presetsAfterFirst);
    QCOMPARE(m_db->listMappingAssignments(kController).size(), assignmentsAfterFirst);
}

// Reviewer correction E ordering, failure half: content may already be written
// (it changes nothing while the source is unverified), the promotion
// transaction is atomic, and a retry after the fault completes the chain.
void PresetMaterializerTest::promotionFailureLeavesNoPartialStateAndRetries()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("xinput.slot1", "global.screenshot", 1, "gamepad.capture", "tap"),
        row("controller-cccc", "global.screenshot", 1, "gamepad.menu", "hold", 1500),
    }));
    QVERIFY(openApp());

    bool ok = false;
    rawExec(QStringLiteral(
        "CREATE TRIGGER block_promote BEFORE UPDATE ON mapping_preset_sources "
        "WHEN NEW.status = 'promoted' "
        "BEGIN SELECT RAISE(ABORT, 'promotion blocked'); END"), &ok);
    QVERIFY(ok);

    const QString profile = QStringLiteral("controller-cccc");
    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("xinput.slot1")}));
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);

    const auto failed = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(failed.verdict), int(MappingPresetMaterializer::Verdict::Failed));
    // Assignment + promoted mark are one transaction; nothing half-landed.
    QVERIFY(m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                .presetId.isEmpty());
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("unverified"));
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));

    // The failure left the chain exactly as retryable as the crash window
    // between content replacement and promotion (correction E): unverified
    // source, no assignment.
    rawExec(QStringLiteral("DROP TRIGGER block_promote"), &ok);
    QVERIFY(ok);

    const auto retry = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(retry.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("promoted"));
    QVERIFY(!m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                 .presetId.isEmpty());
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.menu"));
}

void PresetMaterializerTest::classificationRetriesAfterAFailedSweep()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("controller-bbbb", "global.screenshot", 1, "gamepad.menu", "hold", 1500),
        row("controller-cccc", "global.screenshot", 1, "gamepad.view"),
    }));
    QVERIFY(openApp());

    bool ok = false;
    rawExec(QStringLiteral(
        "CREATE TRIGGER block_retire BEFORE UPDATE ON mapping_preset_sources "
        "WHEN NEW.status = 'retired' "
        "BEGIN SELECT RAISE(ABORT, 'retire blocked'); END"), &ok);
    QVERIFY(ok);

    const QString profile = QStringLiteral("controller-cccc");
    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("controller-bbbb")}));

    const auto first = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(first.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QVERIFY(first.retryNextTime);
    // The proof landed (assignment + activation), the bookkeeping did not.
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("promoted"));
    QCOMPARE(m_db->mappingPresetSource(kController, QStringLiteral("controller-bbbb")).status,
             QStringLiteral("unverified"));
    QVERIFY(m_resolver->isMaterialized(kController, profile));

    rawExec(QStringLiteral("DROP TRIGGER block_retire"), &ok);
    QVERIFY(ok);

    const auto second = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(second.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QVERIFY(!second.retryNextTime);
    QCOMPARE(m_db->mappingPresetSource(kController, QStringLiteral("controller-bbbb")).status,
             QStringLiteral("retired"));
    QVERIFY(m_resolver->isMaterialized(kController, profile));
}

void PresetMaterializerTest::slotAliasRowsFlowIntoTheChainButStaySlotScoped()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("xinput.slot1", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());

    // The slot key was converted to its own slot-scoped target by v9.
    const MappingAssignment slotTarget = m_db->mappingAssignment(
        kController, QStringLiteral("legacy_slot"), QStringLiteral("xinput.slot1"));
    QVERIFY(!slotTarget.presetId.isEmpty());

    const QString profile = QStringLiteral("controller-eeee");
    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("xinput.slot1")}));
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));

    // The chain content carries the slot winner; the slot target is untouched
    // and no controller assignment was ever derived from the slot key text.
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 1),
             QStringLiteral("gamepad.capture"));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    QCOMPARE(m_db->mappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot1"))
                 .presetId,
             slotTarget.presetId);
    QVERIFY(m_db->mappingAssignment(kController, QStringLiteral("controller"),
                                    QStringLiteral("xinput.slot1"))
                .presetId.isEmpty());
}

// Reviewer correction D seam: once content belongs to somebody else (a foreign
// assignment), the migration must stop rewriting it and fall back to legacy.
void PresetMaterializerTest::foreignReferenceRefusesContentRewrite()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("xinput.slot1", "global.screenshot", 2, "gamepad.capture", "tap"),
        row("controller-cccc", "global.screenshot", 1, "gamepad.menu", "hold", 1500),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-cccc");
    const QString converted = m_db->mappingPresetSource(kController, profile).convertedPresetId;
    QVERIFY(!converted.isEmpty());
    // Simulate later user-owned state: another durable controller points at the
    // same preset. The migration may not silently rewrite shared content.
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"),
                                       QStringLiteral("controller-other"), converted));

    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("xinput.slot1")}));
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Refused));
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    QVERIFY(m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                .presetId.isEmpty());
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("unverified"));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    // Legacy lap keeps serving the slot row the content could not carry.
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.screenshot"), 2),
             QStringLiteral("gamepad.capture"));
}

// Keyboard/mouse have no per-device identity: their group default may become
// active right after the offline proof - and only while it keeps holding.
void PresetMaterializerTest::keyboardGroupDefaultActivatesOnlyAfterProof()
{
    QVERIFY(buildLegacyFixture(m_path, {
        keyRow("", "global.screenshot", 2, "Ctrl+Alt+P"),
    }, kKeyboard));
    QVERIFY(openApp());

    // v9 converted the keyboard group-wide rows and the resolver proved them
    // equal at reload() time.
    QVERIFY(m_resolver->isMaterialized(kKeyboard, QString()));
    QCOMPARE(triggerFor(*m_resolver, kKeyboard, QString(), QStringLiteral("global.screenshot"), 2),
             QStringLiteral("Ctrl+Alt+P"));
    const QString migrationPreset =
        m_db->mappingAssignment(kKeyboard, QStringLiteral("group_default"), QString()).presetId;
    QVERIFY(!migrationPreset.isEmpty());
    QCOMPARE(m_db->mappingPreset(migrationPreset).origin, QStringLiteral("migration"));

    // A user-created group default is dormant pre-cpo-p04: the bridge must not
    // activate arbitrary assignments.
    MappingPresetRow userRow;
    userRow.actionId = QStringLiteral("global.screenshot");
    userRow.slot = 2;
    userRow.triggerCode = QStringLiteral("Ctrl+Alt+P");
    const QString userPreset = m_db->createMappingPreset(
        kKeyboard, QStringLiteral("UserDefault"), {userRow});
    QVERIFY(!userPreset.isEmpty());
    QVERIFY(m_db->setMappingAssignment(kKeyboard, QStringLiteral("group_default"), QString(),
                                       userPreset));
    m_resolver->reload();
    QVERIFY(!m_resolver->isMaterialized(kKeyboard, QString()));
    // Legacy still serves the same table, so nothing user-visible moved.
    QCOMPARE(triggerFor(*m_resolver, kKeyboard, QString(), QStringLiteral("global.screenshot"), 2),
             QStringLiteral("Ctrl+Alt+P"));

    QVERIFY(m_db->setMappingAssignment(kKeyboard, QStringLiteral("group_default"), QString(),
                                       migrationPreset));
    m_resolver->reload();
    QVERIFY(m_resolver->isMaterialized(kKeyboard, QString()));
}

// Reviewer correction B: a controller group default in the database must not
// move any pad off legacy resolution - only that pad's own materialized proof
// may switch it.
void PresetMaterializerTest::controllerGroupDefaultDoesNotActivateChains()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.toggle_overlay", 1, "gamepad.guide"),
        row("controller-aaaa", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());

    // v9 created the contract-mandated group default, but no chain switched.
    QVERIFY(!m_db->mappingAssignment(kController, QStringLiteral("group_default"), QString())
                 .presetId.isEmpty());
    QVERIFY(!m_resolver->isMaterialized(kController, QString()));
    QVERIFY(!m_resolver->isMaterialized(kController, QStringLiteral("controller-aaaa")));
    QCOMPARE(countAssignments(*m_db, QStringLiteral("controller")), 0);

    // A pad with no rows of its own still resolves through legacy layering.
    const QString unrelated = QStringLiteral("controller-zzzz");
    QCOMPARE(triggerFor(*m_resolver, kController, unrelated,
                        QStringLiteral("global.toggle_overlay"), 1),
             QStringLiteral("gamepad.guide"));
    QVERIFY(!m_resolver->isMaterialized(kController, unrelated));

    // Only the pad's own proof switches it.
    const QString profile = QStringLiteral("controller-aaaa");
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    m_resolver->setProfileAliases(profile, {});
    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Active));
    QVERIFY(m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
    QVERIFY(!m_resolver->isMaterialized(kController, unrelated));
}

// Review correction A: group-wide rows are inherited behavior, never a
// device-specific override. A durable pad with no rows of its own - and no
// alias carrying any - must not gain a controller preset, source or assignment;
// it keeps resolving through the group layer exactly as before.
void PresetMaterializerTest::groupWideOnlyPadStaysOnLegacyResolution()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.toggle_overlay", 1, "gamepad.guide"),
    }));
    QVERIFY(openApp());

    const QString profile = QStringLiteral("controller-zzzz");
    m_resolver->setProfileAliases(profile, {});
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);

    const auto outcome = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(outcome.verdict), int(MappingPresetMaterializer::Verdict::Inert));

    // Nothing was manufactured: no source, no chain preset, no assignment.
    QVERIFY(m_db->mappingPresetSource(kController, profile).sourceKey.isEmpty());
    QCOMPARE(m_db->listMappingPresets(kController).size(), 1);   // the group Default
    QCOMPARE(countAssignments(*m_db, QStringLiteral("controller")), 0);
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    // The pad still resolves through the inherited group-wide row...
    QCOMPARE(triggerFor(*m_resolver, kController, profile,
                        QStringLiteral("global.toggle_overlay"), 1),
             QStringLiteral("gamepad.guide"));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));

    // ...and a repeat run does not drift into the same wrong write.
    const auto again = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(again.verdict), int(MappingPresetMaterializer::Verdict::Inert));
    QCOMPARE(m_db->listMappingPresets(kController).size(), 1);
    QCOMPARE(countAssignments(*m_db, QStringLiteral("controller")), 0);
}

// Review correction B: the migration preset, its rows and the unverified source
// link are one transaction. A failure at the source write must leave exactly
// zero trace, and the retry must create exactly one preset/source pair.
void PresetMaterializerTest::sourceWriteFailureLeavesNoOrphanPresetAndRetries()
{
    QVERIFY(buildLegacyFixture(m_path, {
        row("", "global.screenshot", 1, "gamepad.guide"),
        row("xinput.slot1", "global.screenshot", 1, "gamepad.capture", "tap"),
    }));
    QVERIFY(openApp());

    bool ok = false;
    rawExec(QStringLiteral(
        "CREATE TRIGGER block_source BEFORE INSERT ON mapping_preset_sources "
        "BEGIN SELECT RAISE(ABORT, 'source blocked'); END"), &ok);
    QVERIFY(ok);

    // The slot key was converted to its own preset by v9; the pad's chain has
    // no source yet, so this really is the first materialization path.
    const QString profile = QStringLiteral("controller-9999");
    QVERIFY(m_resolver->setProfileAliases(profile, {QStringLiteral("xinput.slot1")}));
    const QVector<BindingResolver::Binding> legacy =
        m_resolver->effectiveBindings(kController, profile);
    const int presetsBefore = m_db->listMappingPresets(kController).size();

    const auto failed = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(failed.verdict), int(MappingPresetMaterializer::Verdict::Failed));
    // The chain preset rolled back together with its failed source link, so no
    // orphan preset survives that the retry could duplicate.
    QCOMPARE(m_db->listMappingPresets(kController).size(), presetsBefore);
    QVERIFY(m_db->mappingPresetSource(kController, profile).sourceKey.isEmpty());
    QVERIFY(m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                .presetId.isEmpty());
    QVERIFY(!m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));

    rawExec(QStringLiteral("DROP TRIGGER block_source"), &ok);
    QVERIFY(ok);

    const auto retry = m_materializer->considerControllerChain(profile, true);
    QCOMPARE(int(retry.verdict), int(MappingPresetMaterializer::Verdict::Active));
    // Exactly one new chain preset: the failed attempt left no orphan.
    QCOMPARE(m_db->listMappingPresets(kController).size(), presetsBefore + 1);
    QCOMPARE(m_db->mappingPresetSource(kController, profile).status, QStringLiteral("promoted"));
    QVERIFY(!m_db->mappingAssignment(kController, QStringLiteral("controller"), profile)
                 .presetId.isEmpty());
    QVERIFY(m_resolver->isMaterialized(kController, profile));
    QVERIFY(BindingResolver::chainTablesEqual(
        legacy, m_resolver->effectiveBindings(kController, profile)));
}

QTEST_MAIN(PresetMaterializerTest)
#include "tst_presetmaterializer.moc"
