// Assignment resolution for mapping presets (docs/mapping-presets.md sections 4
// and 7, implemented by cpo-p04).
//
// Every case asserts BOTH the winning preset and the source it came from,
// because "exactly one winner, tell the user where it came from" is the whole
// contract of this leaf. The cases pin:
//
// - the precedence chain game -> controller -> group_default -> builtin;
// - game rules applying per group only;
// - the controller step as a TYPED chain: the caller supplies candidates in
//   precedence order (proven durable identity first, then its compatibility
//   aliases) and each candidate is matched against its declared kind only;
// - kind isolation: a slot candidate never reads a `controller` row and a
//   durable candidate never reads a `legacy_slot` row, so a malformed or stale
//   row in one namespace cannot shadow the other kind's legitimate hit;
// - weak/session-local and untyped candidates never attract a persisted
//   `controller` assignment;
// - keyboard/mouse skipping the controller step entirely;
// - an EMPTY preset winning the game rule as the documented "opt out to built-in
//   defaults for this game" case - never a fall-through;
// - broken assignments (missing preset row, preset of another group) stepping
//   down the chain, reported once and deduplicated;
// - identity never inferred: candidate keys this provider did not present can
//   never attract an assignment;
// - canonical game identity END TO END: an assignment written through the public
//   storage API in one spelling is found by resolution in another equivalent
//   spelling, and never produces two canonically equivalent rows.

#include "input/MappingAssignmentResolver.h"
#include "storage/CaptureDatabase.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

const QString kController = QStringLiteral("controller");
const QString kKeyboard = QStringLiteral("keyboard");
const QString kMouse = QStringLiteral("mouse");

using Candidate = MappingAssignmentResolver::IdentityCandidate;

// The shapes a caller's chain can carry: a proven durable registry identity and
// the slot/provider fingerprints a weaker pad can offer.
const QString kExactKey = QStringLiteral("controller-9f2c0e1d4b7a3568c1d0");
const QString kAliasKey = QStringLiteral("xinput.slot1");
const QString kSlotKey = QStringLiteral("winmm.slot2");

// Provenance is declared by the caller, never guessed from the text: durableCandidate()
// is only ever used for an identity the runtime chain proved a strong,
// generation-free logicalId; slotCandidate() for a slot/provider compatibility identity.
Candidate durableCandidate(const QString& key)
{
    return Candidate{QStringLiteral("controller"), key};
}

Candidate slotCandidate(const QString& key)
{
    return Candidate{QStringLiteral("legacy_slot"), key};
}

MappingPresetRow presetRow(const char* action, const char* trigger)
{
    MappingPresetRow row;
    row.actionId = QString::fromLatin1(action);
    row.triggerCode = QString::fromLatin1(trigger);
    return row;
}

} // namespace

class MappingAssignmentResolverTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void gameRuleWinsAndNamesItsSource();
    void gameRuleAppliesPerGroupOnly();
    void controllerRuleWinsOverGroupDefaultAndFollowsTheChain();
    void slotScopedAssignmentAppliesWithinTheControllerStep();
    void slotCandidateNeverReadsAControllerRow();
    void durableCandidateNeverReadsASlotRow();
    void weakIdentityCannotAttractAControllerAssignment();
    void untypedCandidateKindsAreIgnoredInTheControllerStep();
    void keyboardAndMouseSkipTheControllerStep();
    void builtinIsTheSourceWhenNothingIsAssigned();
    void emptyPresetIsAValidGameWinner();
    void missingPresetStepsDownAndIsReportedOnce();
    void wrongGroupPresetIsStaleAndSkipped();
    void anotherProvidersKeysNeverAttractAnAssignment();
    void gameKeyCanonicalizationIsIdempotentAndCasingInsensitive();
    void gameAssignmentWrittenInAnySpellingIsFoundInAnySpelling();
    void repeatedResolutionIsDeterministic();

private:
    bool openDb();
    void closeDb();
    bool rawExec(const QString& sql);
    QString createPreset(const QString& group, const QString& name);

    QTemporaryDir m_dir;
    QString m_path;
    std::unique_ptr<CaptureDatabase> m_db;
    std::unique_ptr<MappingAssignmentResolver> m_resolver;
};

void MappingAssignmentResolverTest::init()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("gamehq.db"));
    // One fresh database per case: assignments of one test must never leak into
    // another.
    QFile::remove(m_path);
}

void MappingAssignmentResolverTest::cleanup()
{
    closeDb();
}

bool MappingAssignmentResolverTest::openDb()
{
    m_db = std::make_unique<CaptureDatabase>(m_path);
    if (!m_db->open())
        return false;
    m_resolver = std::make_unique<MappingAssignmentResolver>(m_db.get());
    return true;
}

void MappingAssignmentResolverTest::closeDb()
{
    m_resolver.reset();
    m_db.reset();
    QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
}

bool MappingAssignmentResolverTest::rawExec(const QString& sql)
{
    bool ok = false;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("resolver-raw"));
        raw.setDatabaseName(m_path);
        if (raw.open()) {
            QSqlQuery q(raw);
            ok = q.exec(sql);
            raw.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("resolver-raw"));
    return ok;
}

QString MappingAssignmentResolverTest::createPreset(const QString& group, const QString& name)
{
    // Keyboard presets carry no content: resolution never reads preset rows, and
    // the content validator is deliberately group-aware.
    const QVector<MappingPresetRow> rows =
        group == kController
            ? QVector<MappingPresetRow>{presetRow("global.screenshot", "gamepad.capture")}
            : QVector<MappingPresetRow>{};
    return m_db->createMappingPreset(group, name, rows);
}

void MappingAssignmentResolverTest::gameRuleWinsAndNamesItsSource()
{
    QVERIFY(openDb());
    const QString gamePreset = createPreset(kController, QStringLiteral("GamePreset"));
    const QString padPreset = createPreset(kController, QStringLiteral("PadPreset"));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(!gamePreset.isEmpty() && !padPreset.isEmpty() && !groupPreset.isEmpty());

    const QString rawExe = QStringLiteral("C:\\Games\\Racer\\Racer.EXE");
    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(rawExe);
    QVERIFY(!gameKey.isEmpty());
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), gameKey, gamePreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       padPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    const auto result = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QCOMPARE(result.presetId, gamePreset);          // exactly one winner
    QCOMPARE(result.source, QStringLiteral("game")); // ... and where it came from
    QCOMPARE(result.targetKind, QStringLiteral("game"));
    QCOMPARE(result.targetKey, gameKey);
    QVERIFY(result.steppedDown.isEmpty());
    QVERIFY(m_resolver->staleReports().isEmpty());
}

void MappingAssignmentResolverTest::gameRuleAppliesPerGroupOnly()
{
    QVERIFY(openDb());
    const QString keyboardGame = createPreset(kKeyboard, QStringLiteral("KeyboardGame"));
    const QString groupPreset = createPreset(kController, QStringLiteral("ControllerDefault"));
    QVERIFY(!keyboardGame.isEmpty() && !groupPreset.isEmpty());

    const QString rawExe = QStringLiteral("C:\\Games\\Racer.EXE");
    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(rawExe);
    QVERIFY(m_db->setMappingAssignment(kKeyboard, QStringLiteral("game"), gameKey, keyboardGame));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    // The keyboard group's game rule must not leak into the controller group...
    const auto controller = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QCOMPARE(controller.presetId, groupPreset);
    QCOMPARE(controller.source, QStringLiteral("group_default"));

    // ... while the keyboard group itself resolves the game rule.
    const auto keyboard = m_resolver->resolve(kKeyboard, {}, rawExe);
    QCOMPARE(keyboard.presetId, keyboardGame);
    QCOMPARE(keyboard.source, QStringLiteral("game"));
    QCOMPARE(keyboard.targetKind, QStringLiteral("game"));
}

void MappingAssignmentResolverTest::controllerRuleWinsOverGroupDefaultAndFollowsTheChain()
{
    QVERIFY(openDb());
    const QString exactPreset = createPreset(kController, QStringLiteral("ExactPreset"));
    const QString aliasPreset = createPreset(kController, QStringLiteral("AliasPreset"));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(!exactPreset.isEmpty() && !aliasPreset.isEmpty() && !groupPreset.isEmpty());

    const QString aliasDurable = QStringLiteral("controller-77aa66bb55cc44dd33ee");
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       exactPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), aliasDurable,
                                       aliasPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    // The first hit in the caller's chain wins: the exact proven identity is
    // more specific than its alias.
    const auto exact = m_resolver->resolve(kController, {durableCandidate(kExactKey), durableCandidate(aliasDurable)},
                                           QString());
    QCOMPARE(exact.presetId, exactPreset);
    QCOMPARE(exact.source, QStringLiteral("controller"));
    QCOMPARE(exact.targetKind, QStringLiteral("controller"));
    QCOMPARE(exact.targetKey, kExactKey);

    // An alias alone still serves when it is the only candidate present...
    const auto alias = m_resolver->resolve(kController, {durableCandidate(aliasDurable)}, QString());
    QCOMPARE(alias.presetId, aliasPreset);
    QCOMPARE(alias.targetKey, aliasDurable);

    // ... and with nothing assigned to this pad the group default takes over.
    const auto group = m_resolver->resolve(
        kController, {durableCandidate(QStringLiteral("controller-0000000000000000"))}, QString());
    QCOMPARE(group.presetId, groupPreset);
    QCOMPARE(group.source, QStringLiteral("group_default"));
    QCOMPARE(group.targetKind, QStringLiteral("group_default"));
    QVERIFY(group.targetKey.isEmpty());
}

void MappingAssignmentResolverTest::slotScopedAssignmentAppliesWithinTheControllerStep()
{
    QVERIFY(openDb());
    const QString exactPreset = createPreset(kController, QStringLiteral("ExactPreset"));
    const QString slotPreset = createPreset(kController, QStringLiteral("SlotPreset"));
    QVERIFY(!exactPreset.isEmpty() && !slotPreset.isEmpty());

    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       exactPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("legacy_slot"), kSlotKey,
                                       slotPreset));

    // A pad that can only report the slot fingerprint gets the slot-scoped
    // preset; the rule is the controller step, the target kind says "slot".
    const auto asSlotStep = m_resolver->resolve(kController, {slotCandidate(kSlotKey)}, QString());
    QCOMPARE(asSlotStep.presetId, slotPreset);
    QCOMPARE(asSlotStep.source, QStringLiteral("controller"));
    QCOMPARE(asSlotStep.targetKind, QStringLiteral("legacy_slot"));
    QCOMPARE(asSlotStep.targetKey, kSlotKey);

    // Chain order decides between the two targets: the exact durable key is
    // presented before the slot fingerprint.
    const auto exact = m_resolver->resolve(kController, {durableCandidate(kExactKey), slotCandidate(kSlotKey)},
                                           QString());
    QCOMPARE(exact.presetId, exactPreset);
    QCOMPARE(exact.targetKind, QStringLiteral("controller"));
    QCOMPARE(exact.targetKey, kExactKey);
}

void MappingAssignmentResolverTest::slotCandidateNeverReadsAControllerRow()
{
    QVERIFY(openDb());
    const QString forgedPreset = createPreset(kController, QStringLiteral("ForgedDurable"));
    const QString slotPreset = createPreset(kController, QStringLiteral("SlotPreset"));
    QVERIFY(!forgedPreset.isEmpty() && !slotPreset.isEmpty());

    // A malformed/stale `controller` row whose TEXT happens to be the slot
    // fingerprint a weaker pad presents, next to the legitimate slot-scoped row.
    // The caller's chain declares this identity as a slot: the slot assignment
    // wins and the durable-namespace text is never consulted - syntax is not
    // proof, so it cannot shadow the honest row.
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kAliasKey,
                                       forgedPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("legacy_slot"), kAliasKey,
                                       slotPreset));
    QCOMPARE(m_db->mappingAssignment(kController, QStringLiteral("controller"), kAliasKey).presetId,
             forgedPreset);   // the forged row really exists in the database

    const auto result = m_resolver->resolve(kController, {slotCandidate(kAliasKey)}, QString());
    QCOMPARE(result.presetId, slotPreset);
    QCOMPARE(result.source, QStringLiteral("controller"));
    QCOMPARE(result.targetKind, QStringLiteral("legacy_slot"));
    QCOMPARE(result.targetKey, kAliasKey);
    QVERIFY(result.steppedDown.isEmpty());
}

void MappingAssignmentResolverTest::durableCandidateNeverReadsASlotRow()
{
    QVERIFY(openDb());
    const QString durablePreset = createPreset(kController, QStringLiteral("DurablePreset"));
    const QString forgedSlotPreset = createPreset(kController, QStringLiteral("ForgedSlot"));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(!durablePreset.isEmpty() && !forgedSlotPreset.isEmpty() && !groupPreset.isEmpty());

    const QString durableKey = QStringLiteral("controller-1122334455667788aabb");
    // A forged/stale legacy_slot row using the same text as the durable identity.
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("legacy_slot"), durableKey,
                                       forgedSlotPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), durableKey,
                                       durablePreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    // A proven durable candidate may only read the durable row.
    const auto result = m_resolver->resolve(kController, {durableCandidate(durableKey)}, QString());
    QCOMPARE(result.presetId, durablePreset);
    QCOMPARE(result.targetKind, QStringLiteral("controller"));

    // With the durable row gone, the same candidate must NOT fall into the slot
    // row that shares its text: kind isolation keeps the chain honest.
    QVERIFY(m_db->clearMappingAssignment(kController, QStringLiteral("controller"), durableKey));
    const auto stepped = m_resolver->resolve(kController, {durableCandidate(durableKey)}, QString());
    QCOMPARE(stepped.presetId, groupPreset);
    QCOMPARE(stepped.source, QStringLiteral("group_default"));

    // Presented as a slot identity, that same row is reachable - provenance, not
    // the string, decides which namespace is read.
    const auto asSlot = m_resolver->resolve(kController, {slotCandidate(durableKey)}, QString());
    QCOMPARE(asSlot.presetId, forgedSlotPreset);
    QCOMPARE(asSlot.source, QStringLiteral("controller"));
    QCOMPARE(asSlot.targetKind, QStringLiteral("legacy_slot"));
}

void MappingAssignmentResolverTest::weakIdentityCannotAttractAControllerAssignment()
{
    QVERIFY(openDb());
    const QString durablePreset = createPreset(kController, QStringLiteral("DurablePreset"));
    QVERIFY(!durablePreset.isEmpty());

    // A durable assignment exists, but the live chain can only offer the
    // session-local slot fingerprint under the same text - never presented as a
    // `controller` candidate without runtime proof. The durable row stays out of
    // reach and resolution falls through to built-in.
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kAliasKey,
                                       durablePreset));
    const auto result = m_resolver->resolve(kController, {slotCandidate(kAliasKey)}, QString());
    QVERIFY(result.presetId.isEmpty());
    QCOMPARE(result.source, QStringLiteral("builtin"));
    QVERIFY(result.steppedDown.isEmpty());
    QVERIFY(m_resolver->staleReports().isEmpty());
}

void MappingAssignmentResolverTest::untypedCandidateKindsAreIgnoredInTheControllerStep()
{
    QVERIFY(openDb());
    const QString durablePreset = createPreset(kController, QStringLiteral("DurablePreset"));
    const QString gamePreset = createPreset(kController, QStringLiteral("GamePreset"));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(!durablePreset.isEmpty() && !gamePreset.isEmpty() && !groupPreset.isEmpty());

    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       durablePreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));
    // A real `game` row under the same key text, so a resolver that wrongly
    // probed non-controller kinds would visibly serve it.
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), kExactKey, gamePreset));

    // The typed chain carries controller targets only: a game key smuggled into
    // it, or an untyped candidate, is ignored - never probed.
    const auto gameKind = m_resolver->resolve(
        kController, {Candidate{QStringLiteral("game"), kExactKey}}, QString());
    QCOMPARE(gameKind.presetId, groupPreset);
    QCOMPARE(gameKind.source, QStringLiteral("group_default"));

    const auto untyped = m_resolver->resolve(kController, {Candidate{QString(), kExactKey}},
                                             QString());
    QCOMPARE(untyped.presetId, groupPreset);
    QCOMPARE(untyped.source, QStringLiteral("group_default"));

    // The same chain step still serves the durable target when it is declared
    // as what it is.
    const auto proper = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, QString());
    QCOMPARE(proper.presetId, durablePreset);
    QCOMPARE(proper.source, QStringLiteral("controller"));
}

void MappingAssignmentResolverTest::keyboardAndMouseSkipTheControllerStep()
{
    QVERIFY(openDb());
    const QString padPreset = createPreset(kController, QStringLiteral("PadPreset"));
    const QString keyboardPreset = createPreset(kKeyboard, QStringLiteral("KeyboardDefault"));
    QVERIFY(!padPreset.isEmpty() && !keyboardPreset.isEmpty());

    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       padPreset));
    QVERIFY(m_db->setMappingAssignment(kKeyboard, QStringLiteral("group_default"), QString(),
                                       keyboardPreset));

    // The same chain is ignored for the keyboard group: a pad target must never
    // serve keystrokes, not even a perfectly valid one.
    const auto keyboard = m_resolver->resolve(kKeyboard, {durableCandidate(kExactKey)}, QString());
    QCOMPARE(keyboard.presetId, keyboardPreset);
    QCOMPARE(keyboard.source, QStringLiteral("group_default"));

    // ... and the mouse group falls straight through to built-in.
    const auto mouse = m_resolver->resolve(kMouse, {durableCandidate(kExactKey)}, QString());
    QVERIFY(mouse.presetId.isEmpty());
    QCOMPARE(mouse.source, QStringLiteral("builtin"));
}

void MappingAssignmentResolverTest::builtinIsTheSourceWhenNothingIsAssigned()
{
    QVERIFY(openDb());
    const auto controller = m_resolver->resolve(kController, {durableCandidate(kExactKey), slotCandidate(kAliasKey)},
                                                QStringLiteral("C:\\Games\\Racer.EXE"));
    QVERIFY(controller.presetId.isEmpty());
    QCOMPARE(controller.source, QStringLiteral("builtin"));
    QVERIFY(controller.targetKind.isEmpty());
    QVERIFY(controller.targetKey.isEmpty());
    QVERIFY(controller.steppedDown.isEmpty());
    QVERIFY(m_resolver->staleReports().isEmpty());
}

void MappingAssignmentResolverTest::emptyPresetIsAValidGameWinner()
{
    QVERIFY(openDb());
    // Zero rows is the documented "built-in defaults for this game" opt-out.
    const QString optOut = m_db->createMappingPreset(kController, QStringLiteral("BuiltInGame"));
    QVERIFY(!optOut.isEmpty());

    const QString rawExe = QStringLiteral("C:\\Games\\Racer.EXE");
    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(rawExe);
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), gameKey, optOut));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    // The empty preset WINS the game rule: never a fall-through, never reported.
    const auto result = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QCOMPARE(result.presetId, optOut);
    QCOMPARE(result.source, QStringLiteral("game"));
    QCOMPARE(result.targetKind, QStringLiteral("game"));
    QVERIFY(result.steppedDown.isEmpty());
    QVERIFY(m_resolver->staleReports().isEmpty());
}

void MappingAssignmentResolverTest::missingPresetStepsDownAndIsReportedOnce()
{
    QVERIFY(openDb());
    const QString gamePreset = createPreset(kController, QStringLiteral("GamePreset"));
    const QString padPreset = createPreset(kController, QStringLiteral("PadPreset"));
    QVERIFY(!gamePreset.isEmpty() && !padPreset.isEmpty());

    const QString rawExe = QStringLiteral("C:\\Games\\Racer.EXE");
    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(rawExe);
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), gameKey, gamePreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       padPreset));

    QVERIFY(rawExec(QStringLiteral("DELETE FROM mapping_presets WHERE id = '%1'").arg(gamePreset)));
    const auto result = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QCOMPARE(result.presetId, padPreset);
    QCOMPARE(result.source, QStringLiteral("controller"));
    QCOMPARE(result.steppedDown.size(), 1);
    QVERIFY(result.steppedDown.first().contains(QStringLiteral("missing preset")));

    const auto reports = m_resolver->staleReports();
    QCOMPARE(reports.size(), 1);
    QCOMPARE(reports.first().reason, QStringLiteral("missing_preset"));
    QCOMPARE(reports.first().targetKind, QStringLiteral("game"));
    QCOMPARE(reports.first().targetKey, gameKey);
    QCOMPARE(reports.first().presetId, gamePreset);
    QCOMPARE(reports.first().deviceGroup, kController);

    // Reported once: repeated resolutions do not repeat the report.
    m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QCOMPARE(m_resolver->staleReports().size(), 1);

    // A second broken assignment is a second report, and with every rule broken
    // the chain ends at the built-in defaults.
    QVERIFY(rawExec(QStringLiteral("DELETE FROM mapping_presets WHERE id = '%1'").arg(padPreset)));
    const auto exhausted = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    QVERIFY(exhausted.presetId.isEmpty());
    QCOMPARE(exhausted.source, QStringLiteral("builtin"));
    QCOMPARE(exhausted.steppedDown.size(), 2);
    QCOMPARE(m_resolver->staleReports().size(), 2);

    m_resolver->clearStaleReports();
    QVERIFY(m_resolver->staleReports().isEmpty());
}

void MappingAssignmentResolverTest::wrongGroupPresetIsStaleAndSkipped()
{
    QVERIFY(openDb());
    const QString keyboardPreset = createPreset(kKeyboard, QStringLiteral("KeyboardLayout"));
    const QString groupPreset = createPreset(kController, QStringLiteral("GroupPreset"));
    QVERIFY(!keyboardPreset.isEmpty() && !groupPreset.isEmpty());
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(),
                                       groupPreset));

    // Storage refuses to write this pairing; a database damaged outside the app
    // can still hold it, and resolution must treat it as broken rather than
    // serve keyboard content to a pad.
    QVERIFY(rawExec(QStringLiteral(
        "INSERT INTO mapping_assignments "
        "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
        "VALUES ('controller', 'controller', '%1', '%2', NULL, '2026-09-20', '2026-09-20')")
                        .arg(kExactKey, keyboardPreset)));

    const auto result = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, QString());
    QCOMPARE(result.presetId, groupPreset);
    QCOMPARE(result.source, QStringLiteral("group_default"));
    QCOMPARE(result.steppedDown.size(), 1);
    QVERIFY(result.steppedDown.first().contains(QStringLiteral("belongs to group keyboard")));

    const auto reports = m_resolver->staleReports();
    QCOMPARE(reports.size(), 1);
    QCOMPARE(reports.first().reason, QStringLiteral("wrong_group"));
    QCOMPARE(reports.first().targetKind, QStringLiteral("controller"));
    QCOMPARE(reports.first().targetKey, kExactKey);
}

void MappingAssignmentResolverTest::anotherProvidersKeysNeverAttractAnAssignment()
{
    QVERIFY(openDb());
    const QString padPreset = createPreset(kController, QStringLiteral("PadPreset"));
    QVERIFY(!padPreset.isEmpty());
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       padPreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("legacy_slot"), kSlotKey,
                                       padPreset));

    // Exact matching only: another provider's keys are not aliases of this pad's,
    // whatever they look like.
    const auto result = m_resolver->resolve(
        kController,
        {durableCandidate(QStringLiteral("controller-0000111122223333ffff")),
         slotCandidate(QStringLiteral("winmm.slot9"))},
        QString());
    QVERIFY(result.presetId.isEmpty());
    QCOMPARE(result.source, QStringLiteral("builtin"));
}

void MappingAssignmentResolverTest::gameKeyCanonicalizationIsIdempotentAndCasingInsensitive()
{
    // Not on disk: casing and separators still normalize to one deterministic
    // key, because the key is cleaned before it is folded to lower case.
    const QString missing = QStringLiteral("D:\\Games\\Racer\\Racer.EXE");
    const QString missingLower = QStringLiteral("d:/games/racer/racer.exe");
    const QString missingCanonical = MappingAssignmentResolver::canonicalGameKey(missing);
    QVERIFY(!missingCanonical.isEmpty());
    QCOMPARE(missingCanonical, MappingAssignmentResolver::canonicalGameKey(missingLower));
    QCOMPARE(missingCanonical, missingCanonical.toLower());
    QCOMPARE(MappingAssignmentResolver::canonicalGameKey(missingCanonical), missingCanonical);

    // On disk: the real executable resolves to its true on-disk casing, folded
    // to lower case, and the function stays idempotent on its own output.
    const QString exePath = QCoreApplication::applicationFilePath();
    QVERIFY(QFile::exists(exePath));
    const QString canonical = MappingAssignmentResolver::canonicalGameKey(exePath);
    QVERIFY(!canonical.isEmpty());
    QCOMPARE(canonical, canonical.toLower());
    QCOMPARE(MappingAssignmentResolver::canonicalGameKey(canonical), canonical);

    // Nothing normalizes to a nonempty key.
    QVERIFY(MappingAssignmentResolver::canonicalGameKey(QString()).isEmpty());
    QVERIFY(MappingAssignmentResolver::canonicalGameKey(QStringLiteral("   ")).isEmpty());
}

void MappingAssignmentResolverTest::gameAssignmentWrittenInAnySpellingIsFoundInAnySpelling()
{
    QVERIFY(openDb());
    const QString first = createPreset(kController, QStringLiteral("FirstGamePreset"));
    const QString second = createPreset(kController, QStringLiteral("SecondGamePreset"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    // A real executable, so canonicalization exercises the on-disk path — and
    // the writer and the reader are different callers, neither obliged to
    // spell the path the way the other did.
    const QString exe = QCoreApplication::applicationFilePath();
    QVERIFY(QFile::exists(exe));
    const QString writerSpelling = exe.toUpper();
    const QString readerSpelling = QDir::fromNativeSeparators(exe.toLower());
    QVERIFY(writerSpelling != readerSpelling);

    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), writerSpelling, first));

    const QString canonical = MappingAssignmentResolver::canonicalGameKey(exe);
    QVERIFY(!canonical.isEmpty());
    // The row is stored in the canonical form, whatever spelling was written...
    QCOMPARE(m_db->mappingAssignment(kController, QStringLiteral("game"), exe).targetKey,
             canonical);

    // ... so resolution finds the assignment through another equivalent
    // spelling of the same executable.
    const auto result = m_resolver->resolve(kController, {}, readerSpelling);
    QCOMPARE(result.presetId, first);
    QCOMPARE(result.source, QStringLiteral("game"));
    QCOMPARE(result.targetKind, QStringLiteral("game"));
    QCOMPARE(result.targetKey, canonical);

    // Re-writing the same logical game in a third spelling updates that one row
    // instead of creating a canonically equivalent twin.
    const QString thirdSpelling = QDir::toNativeSeparators(exe.toUpper());
    QVERIFY(thirdSpelling != readerSpelling);
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), readerSpelling, second));

    int gameRows = 0;
    for (const MappingAssignment& row : m_db->listMappingAssignments(kController)) {
        if (row.targetKind == QStringLiteral("game")) {
            ++gameRows;
            QCOMPARE(row.targetKey, canonical);
        }
    }
    QCOMPARE(gameRows, 1);

    const auto updated = m_resolver->resolve(kController, {}, thirdSpelling);
    QCOMPARE(updated.presetId, second);
    QCOMPARE(updated.source, QStringLiteral("game"));
    QCOMPARE(updated.targetKey, canonical);
}

void MappingAssignmentResolverTest::repeatedResolutionIsDeterministic()
{
    QVERIFY(openDb());
    const QString padPreset = createPreset(kController, QStringLiteral("PadPreset"));
    const QString gamePreset = createPreset(kController, QStringLiteral("GamePreset"));
    QVERIFY(!padPreset.isEmpty() && !gamePreset.isEmpty());

    const QString rawExe = QStringLiteral("C:\\Games\\Racer.EXE");
    const QString gameKey = MappingAssignmentResolver::canonicalGameKey(rawExe);
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("game"), gameKey, gamePreset));
    QVERIFY(m_db->setMappingAssignment(kController, QStringLiteral("controller"), kExactKey,
                                       padPreset));
    // Break the top rule so the resolution carries a stepped-down note and one
    // stale report; a repeated resolution must reproduce both exactly once.
    QVERIFY(rawExec(QStringLiteral("DELETE FROM mapping_presets WHERE id = '%1'").arg(gamePreset)));

    const auto first = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
    for (int i = 0; i < 3; ++i) {
        const auto again = m_resolver->resolve(kController, {durableCandidate(kExactKey)}, rawExe);
        QCOMPARE(again.presetId, first.presetId);
        QCOMPARE(again.source, first.source);
        QCOMPARE(again.targetKind, first.targetKind);
        QCOMPARE(again.targetKey, first.targetKey);
        QCOMPARE(again.steppedDown, first.steppedDown);
    }
    QCOMPARE(first.presetId, padPreset);
    QCOMPARE(first.source, QStringLiteral("controller"));
    QCOMPARE(m_resolver->staleReports().size(), 1);
}

QTEST_MAIN(MappingAssignmentResolverTest)
#include "tst_mappingassignmentresolver.moc"
