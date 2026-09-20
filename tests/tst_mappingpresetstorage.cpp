// Mapping-preset storage (schema v8, docs/mapping-presets.md).
//
// These tests pin the storage contract, not a resolution rule: a preset's
// identity is its opaque id, its device group is immutable, every assignment
// carries its target kind explicitly, and historical `controller-â€¦` keys wait
// in mapping_preset_sources instead of becoming live controller assignments.
//
// Two families of checks matter most here:
// - what the database itself refuses (foreign keys, uniqueness, check
//   constraints) â€” proven with raw SQL, so "we always remember to validate"
//   is never the guarantee;
// - what a failed write leaves behind â€” proven with an injected trigger that
//   aborts mid-transaction, after some rows of the statement set already ran.

#include "input/ControlId.h"
#include "storage/CaptureDatabase.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

MappingPresetRow makeRow(const QString& action, const QString& trigger, int slot = 1,
                         const QString& activation = QStringLiteral("press"), int tapCount = 1,
                         int holdMs = 0, bool unbound = false)
{
    MappingPresetRow row;
    row.actionId     = action;
    row.triggerCode  = trigger;
    row.slot         = slot;
    row.activation   = activation;
    row.tapCount     = tapCount;
    row.holdMs       = holdMs;
    row.unbound      = unbound;
    return row;
}

MappingPresetRow findRow(const QVector<MappingPresetRow>& rows, const QString& actionId, int slot)
{
    for (const MappingPresetRow& row : rows) {
        if (row.actionId == actionId && row.slot == slot)
            return row;
    }
    return {};
}

// A second connection to the same file, used to write raw SQL (fixtures,
// triggers, foreign-key probes) without going through the storage API.
class RawDatabase
{
public:
    explicit RawDatabase(const QString& path)
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                         QStringLiteral("mappingfixture"));
        m_db.setDatabaseName(path);
        m_open = m_db.open();
        if (m_open) {
            // The app turns enforcement on explicitly; a probe of the foreign
            // keys only proves something with the same pragma.
            QSqlQuery(QStringLiteral("PRAGMA foreign_keys = ON"), m_db);
        }
    }
    ~RawDatabase() { m_db.close(); }

    bool isOpen() const { return m_open; }
    bool exec(const QString& sql)
    {
        QSqlQuery q(m_db);
        const bool ok = q.exec(sql);
        if (!ok)
            m_lastError = q.lastError().text();
        return ok;
    }
    QString lastError() const { return m_lastError; }
    int count(const QString& sql)
    {
        QSqlQuery q(m_db);
        if (!q.exec(sql) || !q.next())
            return -1;
        return q.value(0).toInt();
    }

private:
    QSqlDatabase m_db;
    bool m_open = false;
    QString m_lastError;
};

// Reads a games row id directly: CaptureQueries::listGames() only lists games
// that already have captures, which is not what this test needs.
int gameRowIdFor(const QString& dbPath, const QString& displayName)
{
    int id = -1;
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                     QStringLiteral("mappinggames"));
        raw.setDatabaseName(dbPath);
        if (raw.open()) {
            QSqlQuery q(raw);
            q.prepare(QStringLiteral("SELECT id FROM games WHERE display_name = :name"));
            q.bindValue(QStringLiteral(":name"), displayName);
            if (q.exec() && q.next())
                id = q.value(0).toInt();
            raw.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mappinggames"));
    return id;
}

} // namespace

class MappingPresetStorageTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void createsReadsRenamesAndReplacesAPreset();
    void rejectsInvalidPresetInput();
    void namesAreUniquePerGroupCaseInsensitively();
    void replacesTheWholeSparseRowSet();
    void keepsRawHidTriggerCodesByteForByte();
    void roundTripsEveryAssignmentTargetKind();
    void keepsControllerAndLegacySlotTargetsApart();
    void holdsUnverifiedMigrationSourcesWithoutActivatingThem();
    void promotesASourceAndItsAssignmentAtomically();
    void rollsBackBothHalvesWhenPromotionFails();
    void gameAssignmentMatchesOnTheExecutableKeyOnly();
    void rejectsCrossGroupAndMalformedAssignments();
    void refusesToDeleteAReferencedPreset();
    void deletesAndReassignsEveryReferenceKindAtomically();
    void rollsBackContentAndReferencesWhenAWriteFails();
    void reopeningKeepsTheStoreAndStaysAtTheCurrentVersion();

private:
    QTemporaryDir m_dir;
    QString m_path;
};

void MappingPresetStorageTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_path = m_dir.filePath(QStringLiteral("gamehq.db"));
}

void MappingPresetStorageTest::cleanup()
{
    QSqlDatabase::removeDatabase(QStringLiteral("mappingfixture"));
    QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
    QFile::remove(m_path);
}

void MappingPresetStorageTest::createsReadsRenamesAndReplacesAPreset()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString id = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("  My pad  "),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    QVERIFY(!id.isEmpty());
    QVERIFY(id.startsWith(QStringLiteral("preset-")));

    const MappingPreset created = db.mappingPreset(id);
    QCOMPARE(created.id, id);
    QCOMPARE(created.deviceGroup, QStringLiteral("controller"));
    QCOMPARE(created.name, QStringLiteral("My pad"));   // trimmed on the way in
    QCOMPARE(created.origin, QStringLiteral("user"));
    QVERIFY(!created.createdAt.isEmpty());
    QVERIFY(!created.updatedAt.isEmpty());

    QCOMPARE(db.listMappingPresets(QStringLiteral("controller")).size(), 1);
    QCOMPARE(db.listMappingPresets(QStringLiteral("keyboard")).size(), 0);
    QCOMPARE(db.listMappingPresets().size(), 1);
    QVERIFY(db.mappingPreset(QStringLiteral("preset-missing")).id.isEmpty());

    // Renaming is display-only: the id â€” and therefore every assignment â€” stays.
    QVERIFY(db.renameMappingPreset(id, QStringLiteral("Renamed")));
    QCOMPARE(db.mappingPreset(id).name, QStringLiteral("Renamed"));
    QCOMPARE(db.mappingPreset(id).id, id);
    QVERIFY(!db.renameMappingPreset(id, QStringLiteral("   ")));
    QVERIFY(!db.renameMappingPreset(QStringLiteral("preset-missing"), QStringLiteral("X")));
    QCOMPARE(db.mappingPreset(id).name, QStringLiteral("Renamed"));

    // Row replacement is a whole-set operation, and an empty set is legal
    // (the preset then means "shipped defaults").
    QVERIFY(db.replaceMappingPresetRows(
        id, {makeRow(QStringLiteral("global.save_replay"), QStringLiteral("gamepad.share"), 2)}));
    QCOMPARE(db.mappingPresetRows(id).size(), 1);
    QCOMPARE(db.mappingPresetRows(id).first().actionId, QStringLiteral("global.save_replay"));
    QVERIFY(db.replaceMappingPresetRows(id, {}));
    QVERIFY(db.mappingPresetRows(id).isEmpty());

    QVERIFY(db.deleteMappingPreset(id));
    QVERIFY(db.mappingPreset(id).id.isEmpty());
    QVERIFY(db.mappingPresetRows(id).isEmpty());
}

void MappingPresetStorageTest::rejectsInvalidPresetInput()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const auto presetCount = [&db]() { return db.listMappingPresets().size(); };
    QCOMPARE(presetCount(), 0);

    // Unknown group, empty name, unknown origin: no metadata, no rows.
    QVERIFY(db.createMappingPreset(QStringLiteral("gamepad"), QStringLiteral("X")).isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("   ")).isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("X"), {},
                                   QStringLiteral("imported"))
                .isEmpty());
    QCOMPARE(presetCount(), 0);

    // Invalid rows are refused before anything is written.
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 3)})
                .isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 1,
                                            QStringLiteral("squeeze"))})
                .isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"), QString())})
                .isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 1,
                                            QStringLiteral("tap"), 4)})
                .isEmpty());
    // A press carrying a hold duration â€” and a tap carrying one â€” is exactly the
    // corruption the binding parser rejects; storage must not accept a row the
    // runtime would call malformed.
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 1,
                                            QStringLiteral("press"), 1, 750)})
                .isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 1,
                                            QStringLiteral("tap"), 1, 750)})
                .isEmpty());
    // An unbound row with no trigger code is legal content, not an error.
    const QString unboundOnly = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Unbound only"),
        {makeRow(QStringLiteral("global.screenshot"), QString(), 1, QStringLiteral("press"), 1, 0,
                 true)});
    QVERIFY(!unboundOnly.isEmpty());
    QVERIFY(db.deleteMappingPreset(unboundOnly));
    // An unbound row must not carry a leftover trigger code.
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture"), 1,
                                            QStringLiteral("press"), 1, 0, true)})
                .isEmpty());
    // Duplicate (action, slot) inside one call is a rejected input, not a race.
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.capture")),
                                    makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("gamepad.share"))})
                .isEmpty());
    QCOMPARE(presetCount(), 0);

    const QString id = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("A"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    QVERIFY(!id.isEmpty());
    // The same rejections hold for a replacement, and the stored set survives.
    QVERIFY(!db.replaceMappingPresetRows(
        id, {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"), 9)}));
    QVERIFY(!db.replaceMappingPresetRows(id, {makeRow(QStringLiteral("global.screenshot"),
                                                      QStringLiteral("gamepad.share")),
                                              makeRow(QStringLiteral("global.screenshot"),
                                                      QStringLiteral("gamepad.capture"))}));
    QCOMPARE(db.mappingPresetRows(id).size(), 1);
    QCOMPARE(db.mappingPresetRows(id).first().triggerCode, QStringLiteral("gamepad.capture"));

    // The shared binding grammar, not a stricter local one: a canonical
    // controller chord is legal preset content â€¦
    const QString chorded = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Chorded"),
        {makeRow(QStringLiteral("global.screenshot"),
                 QStringLiteral("chord:v1:gamepad.capture>gamepad.guide"))});
    QVERIFY(!chorded.isEmpty());
    QCOMPARE(db.mappingPresetRows(chorded).size(), 1);
    // â€¦ while the same chord outside the controller group fails, exactly as
    // BindingPattern::isValid decides.
    QVERIFY(db.createMappingPreset(QStringLiteral("keyboard"), QStringLiteral("Keys"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("chord:v1:gamepad.capture>gamepad.guide"))})
                .isEmpty());
    // A chord is press-only and needs two different controls.
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("chord:v1:gamepad.capture>gamepad.capture"))})
                .isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("A"),
                                   {makeRow(QStringLiteral("global.screenshot"),
                                            QStringLiteral("chord:v1:gamepad.capture>gamepad.guide"),
                                            1, QStringLiteral("hold"), 1, 500)})
                .isEmpty());
}

void MappingPresetStorageTest::namesAreUniquePerGroupCaseInsensitively()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString first = db.createMappingPreset(QStringLiteral("controller"),
                                                 QStringLiteral("Ace"));
    QVERIFY(!first.isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral("ace")).isEmpty());
    QVERIFY(db.createMappingPreset(QStringLiteral("controller"), QStringLiteral(" ACE "))
                .isEmpty());

    // A name is only unique inside its group: the keyboard group is unrelated.
    const QString keyboard = db.createMappingPreset(QStringLiteral("keyboard"),
                                                    QStringLiteral("ace"));
    QVERIFY(!keyboard.isEmpty());
    QCOMPARE(db.listMappingPresets(QStringLiteral("controller")).size(), 1);
    QCOMPARE(db.listMappingPresets(QStringLiteral("keyboard")).size(), 1);

    const QString other = db.createMappingPreset(QStringLiteral("controller"),
                                                 QStringLiteral("Bee"));
    QVERIFY(!other.isEmpty());
    QVERIFY(!db.renameMappingPreset(other, QStringLiteral("ACE")));
    QCOMPARE(db.mappingPreset(other).name, QStringLiteral("Bee"));
    // Renaming to the same normalized name is a no-op, not a collision.
    QVERIFY(db.renameMappingPreset(first, QStringLiteral("ACE")));
    QCOMPARE(db.mappingPreset(first).name, QStringLiteral("ACE"));
}

void MappingPresetStorageTest::replacesTheWholeSparseRowSet()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString id = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Gestures"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture")),
         makeRow(QStringLiteral("global.save_replay"), QStringLiteral("gamepad.share"), 1,
                 QStringLiteral("hold"), 1, 3000),
         makeRow(QStringLiteral("global.toggle_overlay"), QStringLiteral("gamepad.guide"), 2,
                 QStringLiteral("tap"), 2)});

    // Replacement is exact: rows absent from the new set are gone, and the
    // gesture fields survive a round trip unchanged.
    QVERIFY(db.replaceMappingPresetRows(
        id, {makeRow(QStringLiteral("desktop.bulk_toggle"), QString(), 1,
                     QStringLiteral("press"), 1, 0, true),
             makeRow(QStringLiteral("global.save_replay"), QStringLiteral("gamepad.share"), 1,
                     QStringLiteral("hold"), 1, 2500)}));

    const QVector<MappingPresetRow> rows = db.mappingPresetRows(id);
    QCOMPARE(rows.size(), 2);

    const MappingPresetRow unbound =
        findRow(rows, QStringLiteral("desktop.bulk_toggle"), 1);
    QVERIFY(unbound.unbound);
    QVERIFY(unbound.triggerCode.isEmpty());
    QCOMPARE(unbound.activation, QStringLiteral("press"));

    const MappingPresetRow hold = findRow(rows, QStringLiteral("global.save_replay"), 1);
    QVERIFY(!hold.unbound);
    QCOMPARE(hold.activation, QStringLiteral("hold"));
    QCOMPARE(hold.holdMs, 2500);
    QCOMPARE(hold.triggerCode, QStringLiteral("gamepad.share"));

    // The multi-tap row is gone with the set it belonged to â€” not merged in.
    QCOMPARE(findRow(rows, QStringLiteral("global.toggle_overlay"), 2).actionId, QString());
    QVERIFY(!db.mappingPreset(id).updatedAt.isEmpty());

    // An empty replacement is a legal whole-set operation on a real preset â€¦
    QVERIFY(db.replaceMappingPresetRows(id, {}));
    QVERIFY(db.mappingPresetRows(id).isEmpty());
    // â€¦ but on a missing preset it is a failure, empty set or not: the metadata
    // row is the existence check, so nothing here may report success.
    QVERIFY(!db.replaceMappingPresetRows(QStringLiteral("preset-missing"), {}));
    QVERIFY(!db.replaceMappingPresetRows(QStringLiteral("preset-missing"),
                                         {makeRow(QStringLiteral("global.screenshot"),
                                                  QStringLiteral("gamepad.capture"))}));
    QVERIFY(!db.replaceMappingPresetRows(QString(), {}));
}

void MappingPresetStorageTest::keepsRawHidTriggerCodesByteForByte()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    // A raw-HID code embeds an anonymized endpoint identity, so it only works
    // on that pad. Storage keeps it exactly as written; portability is marked
    // elsewhere (docs/mapping-presets.md section 9).
    const QString rawCode = ControlId::rawHidUsage(
        QStringLiteral("HID\\VID_054C&PID_0CE6&MI_03"), 0x01, 0x09);
    QVERIFY(ControlId::isRawHidUsage(rawCode));

    const QString id = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Bound pad"),
        {makeRow(QStringLiteral("global.screenshot"), rawCode, 1, QStringLiteral("tap"), 1)});

    const QVector<MappingPresetRow> rows = db.mappingPresetRows(id);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().triggerCode, rawCode);
    QCOMPARE(rows.first().triggerCode.toUtf8(), rawCode.toUtf8());
}

void MappingPresetStorageTest::roundTripsEveryAssignmentTargetKind()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString padPreset = db.createMappingPreset(QStringLiteral("controller"),
                                                     QStringLiteral("Pad"));
    const QString slotPreset = db.createMappingPreset(QStringLiteral("controller"),
                                                      QStringLiteral("Slot"));
    const QString keyboardPreset = db.createMappingPreset(QStringLiteral("keyboard"),
                                                          QStringLiteral("Keys"));
    QVERIFY(!padPreset.isEmpty() && !slotPreset.isEmpty() && !keyboardPreset.isEmpty());

    const QString padKey = QStringLiteral("controller-0123456789abcdef");
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    padKey, padPreset));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                    QStringLiteral("xinput.slot1"), slotPreset));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"),
                                    QStringLiteral("d:/games/foo/foo.exe"), padPreset,
                                    7));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"),
                                    QStringLiteral("group_default"), QString(), slotPreset));
    QVERIFY(db.setMappingAssignment(QStringLiteral("keyboard"), QStringLiteral("group_default"),
                                    QString(), keyboardPreset));

    const MappingAssignment pad = db.mappingAssignment(QStringLiteral("controller"),
                                                       QStringLiteral("controller"), padKey);
    QCOMPARE(pad.presetId, padPreset);
    QCOMPARE(pad.targetKind, QStringLiteral("controller"));
    QCOMPARE(pad.targetKey, padKey);
    QCOMPARE(pad.gameRowId, -1);

    const MappingAssignment slot = db.mappingAssignment(QStringLiteral("controller"),
                                                        QStringLiteral("legacy_slot"),
                                                        QStringLiteral("xinput.slot1"));
    QCOMPARE(slot.presetId, slotPreset);
    QCOMPARE(slot.targetKey, QStringLiteral("xinput.slot1"));
    // Slot-scoped, and never presented as a physical controller.
    QCOMPARE(slot.targetKind, QStringLiteral("legacy_slot"));

    const MappingAssignment game = db.mappingAssignment(
        QStringLiteral("controller"), QStringLiteral("game"),
        QStringLiteral("d:/games/foo/foo.exe"));
    QCOMPARE(game.presetId, padPreset);
    QCOMPARE(game.gameRowId, 7);   // cache only; the key above is what matches

    const MappingAssignment groupDefault = db.mappingAssignment(
        QStringLiteral("controller"), QStringLiteral("group_default"), QString());
    QCOMPARE(groupDefault.presetId, slotPreset);
    QVERIFY(groupDefault.targetKey.isEmpty());
    const MappingAssignment keyboardDefault = db.mappingAssignment(
        QStringLiteral("keyboard"), QStringLiteral("group_default"), QString());
    QCOMPARE(keyboardDefault.presetId, keyboardPreset);

    QCOMPARE(db.listMappingAssignments().size(), 5);
    QCOMPARE(db.listMappingAssignments(QStringLiteral("keyboard")).size(), 1);

    // One preset can serve several targets at once; re-pointing a target
    // updates that assignment instead of adding a second one.
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    QStringLiteral("controller-ffffffffffffffff"), padPreset));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                    QStringLiteral("xinput.slot1"), padPreset));
    QCOMPARE(db.listMappingAssignments(QStringLiteral("controller")).size(), 5);
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                  QStringLiteral("xinput.slot1")).presetId, padPreset);

    // Clearing the group default returns the group to built-in defaults: the
    // state is represented by the absence of a row, never by a magic preset.
    QVERIFY(db.clearMappingAssignment(QStringLiteral("controller"),
                                      QStringLiteral("group_default"), QString()));
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("group_default"),
                                 QString())
                .presetId.isEmpty());
    QCOMPARE(db.listMappingAssignments(QStringLiteral("controller")).size(), 4);
    // Clearing again is idempotent.
    QVERIFY(db.clearMappingAssignment(QStringLiteral("controller"),
                                      QStringLiteral("group_default"), QString()));
}

void MappingPresetStorageTest::keepsControllerAndLegacySlotTargetsApart()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString durablePreset = db.createMappingPreset(QStringLiteral("controller"),
                                                         QStringLiteral("Durable"));
    const QString slotPreset = db.createMappingPreset(QStringLiteral("controller"),
                                                      QStringLiteral("Slot"));
    QVERIFY(!durablePreset.isEmpty() && !slotPreset.isEmpty());

    // A historical opaque key is exactly the case where the text cannot say
    // what it means: the same string under two kinds must stay two records.
    const QString sameText = QStringLiteral("controller-0123456789abcdef");
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    sameText, durablePreset));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                    sameText, slotPreset));

    const QVector<MappingAssignment> assignmentRows =
        db.listMappingAssignments(QStringLiteral("controller"));
    QCOMPARE(assignmentRows.size(), 2);
    QVERIFY(assignmentRows.at(0).id != assignmentRows.at(1).id);
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                  sameText).presetId, durablePreset);
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                  sameText).presetId, slotPreset);

    // The two kinds are independently removable.
    QVERIFY(db.clearMappingAssignment(QStringLiteral("controller"),
                                      QStringLiteral("legacy_slot"), sameText));
    QCOMPARE(db.listMappingAssignments(QStringLiteral("controller")).size(), 1);
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                  sameText).presetId, durablePreset);
}

void MappingPresetStorageTest::holdsUnverifiedMigrationSourcesWithoutActivatingThem()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString converted = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Default"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))},
        QStringLiteral("migration"));
    QCOMPARE(db.mappingPreset(converted).origin, QStringLiteral("migration"));
    const QString other = db.createMappingPreset(QStringLiteral("controller"),
                                                 QStringLiteral("Other"));
    QVERIFY(!converted.isEmpty() && !other.isEmpty());

    const QString sourceKey = QStringLiteral("controller-0123456789abcdef");
    MappingPresetSource source;
    source.deviceGroup       = QStringLiteral("controller");
    source.sourceKey         = sourceKey;
    source.convertedPresetId = converted;
    source.note              = QStringLiteral("converted, durability unproven");
    QVERIFY(db.upsertMappingPresetSource(source));

    // The whole point: a migrated key is *not* a live controller assignment.
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                 sourceKey)
                .presetId.isEmpty());
    QVERIFY(db.listMappingAssignments(QStringLiteral("controller")).isEmpty());

    const MappingPresetSource stored =
        db.mappingPresetSource(QStringLiteral("controller"), sourceKey);
    QCOMPARE(stored.sourceKey, sourceKey);
    QCOMPARE(stored.convertedPresetId, converted);
    QCOMPARE(stored.status, QStringLiteral("unverified"));
    QVERIFY(stored.promotedAt.isEmpty());
    QVERIFY(!stored.createdAt.isEmpty());
    QCOMPARE(db.listMappingPresetSources().size(), 1);

    // The lifecycle of an unproven source: it can be retired or kept, but it can
    // never be promoted from here â€” `promoted` belongs to the atomic runtime
    // path, and until that runs the key is not a live controller target.
    QVERIFY(!db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                             QStringLiteral("promoted")));
    QVERIFY(db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                            QStringLiteral("retired")));
    QCOMPARE(db.mappingPresetSource(QStringLiteral("controller"), sourceKey).status,
             QStringLiteral("retired"));
    QVERIFY(db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                            QStringLiteral("unverified")));
    QVERIFY(!db.setMappingPresetSourceStatus(QStringLiteral("controller"),
                                             QStringLiteral("controller-ffffffffffffffff"),
                                             QStringLiteral("retired")));
    QVERIFY(!db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                             QStringLiteral("guessed")));
    QVERIFY(db.mappingPresetSource(QStringLiteral("keyboard"), sourceKey).sourceKey.isEmpty());
    // The generic upsert cannot smuggle a promotion in either: a `promoted`
    // status is refused and nothing about the stored row changes.
    MappingPresetSource smuggled = source;
    smuggled.status     = QStringLiteral("promoted");
    smuggled.promotedAt = QStringLiteral("2026-09-20T10:00:00Z");
    QVERIFY(!db.upsertMappingPresetSource(smuggled));
    QCOMPARE(db.mappingPresetSource(QStringLiteral("controller"), sourceKey).status,
             QStringLiteral("unverified"));
    QVERIFY(db.mappingPresetSource(QStringLiteral("controller"), sourceKey).promotedAt.isEmpty());
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                 sourceKey)
                .presetId.isEmpty());

    // Converted content is recovery evidence: a source keeps it alive, so a
    // delete is refused (with or without a reassignment target).
    QCOMPARE(db.mappingPresetReferenceCount(converted), 1);
    QVERIFY(!db.deleteMappingPreset(converted));
    QVERIFY(!db.deleteMappingPresetAndReassign(converted, other));
    QVERIFY(!db.mappingPreset(converted).id.isEmpty());
    QVERIFY(!db.mappingPresetRows(converted).isEmpty());

    // A source may be recorded even before its conversion produced content.
    MappingPresetSource pending;
    pending.deviceGroup = QStringLiteral("controller");
    pending.sourceKey   = QStringLiteral("controller-fedcba9876543210");
    QVERIFY(db.upsertMappingPresetSource(pending));
    QCOMPARE(db.mappingPresetSource(QStringLiteral("controller"), pending.sourceKey).status,
             QStringLiteral("unverified"));
    // Wrong group for a converted preset is refused.
    MappingPresetSource mismatched;
    mismatched.deviceGroup       = QStringLiteral("keyboard");
    mismatched.sourceKey         = QStringLiteral("keyboard-1234567890abcdef");
    mismatched.convertedPresetId = converted;
    QVERIFY(!db.upsertMappingPresetSource(mismatched));
}

void MappingPresetStorageTest::promotesASourceAndItsAssignmentAtomically()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString preset = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Proven"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    QVERIFY(!preset.isEmpty());

    const QString sourceKey = QStringLiteral("controller-0123456789abcdef");
    MappingPresetSource source;
    source.deviceGroup       = QStringLiteral("controller");
    source.sourceKey         = sourceKey;
    source.convertedPresetId = preset;
    source.note              = QStringLiteral("converted, durability unproven");
    QVERIFY(db.upsertMappingPresetSource(source));

    // Nothing is promotable before runtime proof: unknown source, no key, wrong
    // group â€” and none of the refusals may write a target.
    QVERIFY(!db.promoteMappingPresetSource(
        QStringLiteral("controller"), QStringLiteral("controller-ffffffffffffffff"),
        QStringLiteral("controller-aaaaaaaaaaaaaaaa")));
    QVERIFY(!db.promoteMappingPresetSource(QStringLiteral("controller"), sourceKey, QString()));
    QVERIFY(!db.promoteMappingPresetSource(QStringLiteral("keyboard"), sourceKey,
                                           QStringLiteral("controller-aaaaaaaaaaaaaaaa")));
    QVERIFY(db.listMappingAssignments().isEmpty());
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                 QStringLiteral("controller-aaaaaaaaaaaaaaaa"))
                .presetId.isEmpty());

    // A source without converted content has nothing to activate.
    MappingPresetSource pending;
    pending.deviceGroup = QStringLiteral("controller");
    pending.sourceKey   = QStringLiteral("controller-fedcba9876543210");
    QVERIFY(db.upsertMappingPresetSource(pending));
    QVERIFY(!db.promoteMappingPresetSource(QStringLiteral("controller"), pending.sourceKey,
                                           QStringLiteral("controller-cccccccccccccccc")));
    QVERIFY(db.listMappingAssignments().isEmpty());

    // The authorized transition: the durable assignment and the promoted
    // bookkeeping land as one unit.
    QVERIFY(db.promoteMappingPresetSource(QStringLiteral("controller"), sourceKey,
                                          QStringLiteral("controller-aaaaaaaaaaaaaaaa")));
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                  QStringLiteral("controller-aaaaaaaaaaaaaaaa"))
                 .presetId,
             preset);
    const MappingPresetSource promoted =
        db.mappingPresetSource(QStringLiteral("controller"), sourceKey);
    QCOMPARE(promoted.status, QStringLiteral("promoted"));
    QVERIFY(!promoted.promotedAt.isEmpty());
    QCOMPARE(promoted.convertedPresetId, preset);

    // Promotion is terminal for this leaf: no re-promotion onto a second key, no
    // generic downgrade, no generic relink of the evidence.
    QVERIFY(!db.promoteMappingPresetSource(QStringLiteral("controller"), sourceKey,
                                           QStringLiteral("controller-bbbbbbbbbbbbbbbb")));
    QVERIFY(!db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                             QStringLiteral("unverified")));
    QVERIFY(!db.setMappingPresetSourceStatus(QStringLiteral("controller"), sourceKey,
                                             QStringLiteral("retired")));
    MappingPresetSource relink = source;
    relink.note = QStringLiteral("relinked later");
    QVERIFY(!db.upsertMappingPresetSource(relink));
    QCOMPARE(db.mappingPresetSource(QStringLiteral("controller"), sourceKey).note,
             QStringLiteral("converted, durability unproven"));
    QCOMPARE(db.listMappingAssignments().size(), 1);
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                 QStringLiteral("controller-bbbbbbbbbbbbbbbb"))
                .presetId.isEmpty());
}

void MappingPresetStorageTest::rollsBackBothHalvesWhenPromotionFails()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString preset = db.createMappingPreset(QStringLiteral("controller"),
                                                  QStringLiteral("Half"));
    QVERIFY(!preset.isEmpty());
    const QString sourceKey = QStringLiteral("controller-1111222233334444");
    MappingPresetSource source;
    source.deviceGroup       = QStringLiteral("controller");
    source.sourceKey         = sourceKey;
    source.convertedPresetId = preset;
    QVERIFY(db.upsertMappingPresetSource(source));

    // Injected failure on the SECOND half: the assignment row has already been
    // written inside the transaction when the source update aborts, so a missing
    // rollback would leave exactly the state rev 3 forbids â€” an active controller
    // target with an unverified source, or a promoted source without a target.
    RawDatabase raw(m_path);
    QVERIFY(raw.isOpen());
    QVERIFY(raw.exec(QStringLiteral(
        "CREATE TRIGGER mapping_promote_boom BEFORE UPDATE ON mapping_preset_sources "
        "WHEN NEW.status = 'promoted' BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
    QVERIFY(!db.promoteMappingPresetSource(QStringLiteral("controller"), sourceKey,
                                           QStringLiteral("controller-dddddddddddddddd")));
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                 QStringLiteral("controller-dddddddddddddddd"))
                .presetId.isEmpty());
    QCOMPARE(db.listMappingAssignments().size(), 0);
    const MappingPresetSource still =
        db.mappingPresetSource(QStringLiteral("controller"), sourceKey);
    QCOMPARE(still.status, QStringLiteral("unverified"));
    QVERIFY(still.promotedAt.isEmpty());

    // With the injection gone, the same call lands both halves together.
    QVERIFY(raw.exec(QStringLiteral("DROP TRIGGER mapping_promote_boom")));
    QVERIFY(db.promoteMappingPresetSource(QStringLiteral("controller"), sourceKey,
                                          QStringLiteral("controller-dddddddddddddddd")));
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                  QStringLiteral("controller-dddddddddddddddd"))
                 .presetId,
             preset);
    QCOMPARE(db.mappingPresetSource(QStringLiteral("controller"), sourceKey).status,
             QStringLiteral("promoted"));
}

void MappingPresetStorageTest::gameAssignmentMatchesOnTheExecutableKeyOnly()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString preset = db.createMappingPreset(QStringLiteral("controller"),
                                                  QStringLiteral("For game"));
    QVERIFY(!preset.isEmpty());

    // `rememberGameExecutable` only accepts paths that exist on disk.
    const QString fooPath = QDir(m_dir.path()).filePath(QStringLiteral("foo.exe"));
    const QString barPath = QDir(m_dir.path()).filePath(QStringLiteral("bar.exe"));
    for (const QString& path : {fooPath, barPath}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("stub") > 0);
        file.close();
    }

    QVERIFY(db.rememberGameExecutable(QStringLiteral("Foo"), fooPath));
    // The key is the canonical executable identity the game services hand out.
    const QString exeKey = fooPath;
    const int gameRowId = gameRowIdFor(m_path, QStringLiteral("Foo"));
    QVERIFY(gameRowId >= 0);

    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), exeKey,
                                    preset, gameRowId));
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), exeKey)
                 .gameRowId,
             gameRowId);

    // GameRowRepair may merge or delete game rows. The assignment must keep
    // matching on the executable key, and the cached id must never be able to
    // attract it to a different game.
    {
        RawDatabase raw(m_path);
        QVERIFY(raw.isOpen());
        QVERIFY(raw.exec(QStringLiteral("DELETE FROM games WHERE id = %1").arg(gameRowId)));
    }
    QCOMPARE(gameRowIdFor(m_path, QStringLiteral("Foo")), -1);

    QVERIFY(db.rememberGameExecutable(QStringLiteral("Bar"), barPath));
    QVERIFY(gameRowIdFor(m_path, QStringLiteral("Bar")) >= 0);   // may or may not reuse the deleted id â€” either way:

    const MappingAssignment still =
        db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), exeKey);
    QCOMPARE(still.presetId, preset);
    QCOMPARE(still.gameRowId, gameRowId);   // stale cache, harmless by design
    QCOMPARE(db.listMappingAssignments(QStringLiteral("controller")).size(), 1);

    // Nothing matches a key that was never assigned, whatever game rows exist
    // and whichever row ids they hold.
    QVERIFY(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("game"), barPath)
                .presetId.isEmpty());
}

void MappingPresetStorageTest::rejectsCrossGroupAndMalformedAssignments()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString controllerPreset = db.createMappingPreset(QStringLiteral("controller"),
                                                            QStringLiteral("Pad"));
    const QString keyboardPreset = db.createMappingPreset(QStringLiteral("keyboard"),
                                                          QStringLiteral("Keys"));
    QVERIFY(!controllerPreset.isEmpty() && !keyboardPreset.isEmpty());

    QVERIFY(!db.setMappingAssignment(QStringLiteral("keyboard"), QStringLiteral("group_default"),
                                     QString(), controllerPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                     QStringLiteral("controller-aaaaaaaaaaaaaaaa"),
                                     keyboardPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("keyboard"), QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), keyboardPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"),
                                     QString(), controllerPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("controller"),
                                     QStringLiteral("group_default"),
                                     QStringLiteral("not-empty"), controllerPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("mouse"), QStringLiteral("bogus"),
                                     QStringLiteral("k"), controllerPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("gamepad"),
                                     QStringLiteral("group_default"), QString(), controllerPreset));
    QVERIFY(!db.setMappingAssignment(QStringLiteral("controller"),
                                     QStringLiteral("group_default"), QString(),
                                     QStringLiteral("preset-missing")));
    QCOMPARE(db.listMappingAssignments().size(), 0);

    // The same rules hold in the database itself: with an existing controller
    // preset, a raw cross-group insert is refused by the composite foreign key,
    // and a key on a group default by the check constraint.
    {
        RawDatabase raw(m_path);
        QVERIFY(raw.isOpen());
        QVERIFY(!raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('keyboard','group_default','','%1',NULL,'now','now')")
                              .arg(controllerPreset)));
        QVERIFY(!raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('controller','group_default','x','%1',NULL,'now','now')")
                              .arg(controllerPreset)));
        QVERIFY(raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('controller','group_default','','%1',NULL,'now','now')")
                             .arg(controllerPreset)));
        // The games row-id cache belongs to `game` targets only; raw SQL cannot
        // park one on a pad target (controller alias or group default) â€¦
        QVERIFY(!raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('controller','controller','controller-bbbbbbbbbbbbbbbb','%1',7,'now','now')")
                              .arg(controllerPreset)));
        QVERIFY(!raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('controller','group_default','','%1',7,'now','now')")
                              .arg(controllerPreset)));
        // â€¦ while a game target legitimately keeps its join cache.
        QVERIFY(raw.exec(QStringLiteral(
            "INSERT INTO mapping_assignments "
            "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
            "VALUES ('controller','game','C:/games/foo.exe','%1',7,'now','now')")
                             .arg(controllerPreset)));
    }
    QCOMPARE(db.listMappingAssignments().size(), 2);
}

void MappingPresetStorageTest::refusesToDeleteAReferencedPreset()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString referenced = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Referenced"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    const QString unreferenced = db.createMappingPreset(QStringLiteral("controller"),
                                                        QStringLiteral("Free"));
    const QString keyboard = db.createMappingPreset(QStringLiteral("keyboard"),
                                                    QStringLiteral("Keys"));
    QVERIFY(!referenced.isEmpty() && !unreferenced.isEmpty() && !keyboard.isEmpty());

    // Every target kind participates: none of these deletes may slip through.
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    QStringLiteral("controller-1111111111111111"), referenced));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                    QStringLiteral("xinput.slot0"), referenced));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"),
                                    QStringLiteral("d:/games/foo/foo.exe"), referenced));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"),
                                    QStringLiteral("group_default"), QString(), referenced));

    QCOMPARE(db.mappingPresetReferenceCount(referenced), 4);
    QVERIFY(!db.deleteMappingPreset(referenced));
    QVERIFY(!db.mappingPreset(referenced).id.isEmpty());
    QCOMPARE(db.mappingPresetRows(referenced).size(), 1);
    QCOMPARE(db.listMappingAssignments(QStringLiteral("controller")).size(), 4);

    // An unreferenced preset deletes cleanly, rows and metadata together.
    QCOMPARE(db.mappingPresetReferenceCount(unreferenced), 0);
    QVERIFY(db.deleteMappingPreset(unreferenced));
    QVERIFY(db.mappingPreset(unreferenced).id.isEmpty());
    QVERIFY(db.listMappingPresets(QStringLiteral("controller")).size() == 1);

    // Deleting an unknown preset is refused, not silently "successful".
    QVERIFY(!db.deleteMappingPreset(QStringLiteral("preset-missing")));
    QVERIFY(db.deleteMappingPreset(keyboard));
}

void MappingPresetStorageTest::deletesAndReassignsEveryReferenceKindAtomically()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString victim = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Old"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    const QString keep = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("New"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.share"))});
    const QString keyboardKeep = db.createMappingPreset(QStringLiteral("keyboard"),
                                                        QStringLiteral("Keys"));
    QVERIFY(!victim.isEmpty() && !keep.isEmpty() && !keyboardKeep.isEmpty());

    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    QStringLiteral("controller-2222222222222222"), victim));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("legacy_slot"),
                                    QStringLiteral("xinput.slot1"), victim));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("game"),
                                    QStringLiteral("d:/games/foo/foo.exe"), victim));
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"),
                                    QStringLiteral("group_default"), QString(), victim));
    // An assignment that is not part of the move must not be touched.
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    QStringLiteral("controller-3333333333333333"), keep));

    // Cross-group "reassignment" is not a thing: the target preset must be in
    // the same device group.
    QVERIFY(!db.deleteMappingPresetAndReassign(victim, keyboardKeep));
    QVERIFY(!db.deleteMappingPresetAndReassign(victim, victim));
    QVERIFY(!db.deleteMappingPresetAndReassign(victim, QStringLiteral("preset-missing")));
    QCOMPARE(db.mappingPresetReferenceCount(victim), 4);

    QVERIFY(db.deleteMappingPresetAndReassign(victim, keep));
    QVERIFY(db.mappingPreset(victim).id.isEmpty());
    QVERIFY(db.mappingPresetRows(victim).isEmpty());

    const QVector<MappingAssignment> assignments =
        db.listMappingAssignments(QStringLiteral("controller"));
    QCOMPARE(assignments.size(), 5);
    int moved = 0;
    for (const MappingAssignment& assignment : assignments) {
        if (assignment.targetKey == QStringLiteral("controller-3333333333333333")) {
            QCOMPARE(assignment.presetId, keep);
            continue;
        }
        QCOMPARE(assignment.presetId, keep);   // every former victim target now points at keep
        ++moved;
    }
    QCOMPARE(moved, 4);
    QCOMPARE(db.mappingPresetReferenceCount(keep), 5);

    // The reassigned group default is still a group default, not a controller.
    const MappingAssignment groupDefault = db.mappingAssignment(
        QStringLiteral("controller"), QStringLiteral("group_default"), QString());
    QCOMPARE(groupDefault.presetId, keep);
    QVERIFY(groupDefault.targetKey.isEmpty());
}

void MappingPresetStorageTest::rollsBackContentAndReferencesWhenAWriteFails()
{
    CaptureDatabase db(m_path);
    QVERIFY(db.open());

    const QString victim = db.createMappingPreset(
        QStringLiteral("controller"), QStringLiteral("Victim"),
        {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
    const QString keep = db.createMappingPreset(QStringLiteral("controller"),
                                                QStringLiteral("Keep"));
    QVERIFY(!victim.isEmpty() && !keep.isEmpty());
    QVERIFY(db.setMappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                    QStringLiteral("controller-4444444444444444"), victim));

    // Injected failures: a trigger aborts after the transaction has already
    // written something, which is what makes the rollback observable.
    {
        RawDatabase raw(m_path);
        QVERIFY(raw.isOpen());
        QVERIFY(raw.exec(QStringLiteral(
            "CREATE TRIGGER mapping_row_boom BEFORE INSERT ON mapping_preset_rows "
            "WHEN NEW.action_id = 'test.boom' BEGIN SELECT RAISE(ABORT, 'injected failure'); END")));
        QVERIFY(raw.exec(QStringLiteral(
            "CREATE TRIGGER mapping_move_boom BEFORE UPDATE ON mapping_assignments "
            "WHEN NEW.preset_id = '%1' BEGIN SELECT RAISE(ABORT, 'injected failure'); END")
                             .arg(keep)));
    }

    // 1) A failed row replacement leaves the previous content exactly as it was.
    const QVector<MappingPresetRow> before = db.mappingPresetRows(victim);
    QCOMPARE(before.size(), 1);
    QVERIFY(!db.replaceMappingPresetRows(
        victim, {makeRow(QStringLiteral("global.save_replay"), QStringLiteral("gamepad.share")),
                 makeRow(QStringLiteral("test.boom"), QStringLiteral("gamepad.capture"))}));
    const QVector<MappingPresetRow> after = db.mappingPresetRows(victim);
    QCOMPARE(after.size(), before.size());
    QCOMPARE(after.first().actionId, before.first().actionId);
    QCOMPARE(after.first().triggerCode, before.first().triggerCode);

    // 2) A failed create leaves no orphan metadata behind.
    const int presetCount = db.listMappingPresets(QStringLiteral("controller")).size();
    QVERIFY(db.createMappingPreset(
                QStringLiteral("controller"), QStringLiteral("Half written"),
                {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture")),
                 makeRow(QStringLiteral("test.boom"), QStringLiteral("gamepad.share"))})
                .isEmpty());
    QCOMPARE(db.listMappingPresets(QStringLiteral("controller")).size(), presetCount);

    // 3) A failed delete+reassign leaves both the preset and every reference
    //    untouched â€” no dangling target, no half-moved sweep.
    QVERIFY(!db.deleteMappingPresetAndReassign(victim, keep));
    QVERIFY(!db.mappingPreset(victim).id.isEmpty());
    QCOMPARE(db.mappingPresetReferenceCount(victim), 1);
    QCOMPARE(db.mappingAssignment(QStringLiteral("controller"), QStringLiteral("controller"),
                                  QStringLiteral("controller-4444444444444444"))
                 .presetId,
             victim);
    QCOMPARE(db.mappingPresetReferenceCount(keep), 0);
}

void MappingPresetStorageTest::reopeningKeepsTheStoreAndStaysAtTheCurrentVersion()
{
    QString id;
    {
        CaptureDatabase db(m_path);
        QVERIFY(db.open());
        QCOMPARE(db.schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);
        id = db.createMappingPreset(
            QStringLiteral("controller"), QStringLiteral("Reopen"),
            {makeRow(QStringLiteral("global.screenshot"), QStringLiteral("gamepad.capture"))});
        QVERIFY(!id.isEmpty());
        QVERIFY(db.setMappingAssignment(QStringLiteral("controller"),
                                        QStringLiteral("group_default"), QString(), id));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));

    // Migrations are idempotent: the second open finds version 8 and only
    // re-runs no-op CREATE TABLE IF NOT EXISTS statements.
    {
        CaptureDatabase again(m_path);
        QVERIFY(again.open());
        QCOMPARE(again.schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);
        QCOMPARE(again.mappingPreset(id).name, QStringLiteral("Reopen"));
        QCOMPARE(again.mappingPresetRows(id).size(), 1);
        QCOMPARE(again.mappingAssignment(QStringLiteral("controller"),
                                         QStringLiteral("group_default"), QString())
                     .presetId,
                 id);
        QCOMPARE(again.listMappingPresets().size(), 1);
    }
}

QTEST_MAIN(MappingPresetStorageTest)
#include "tst_mappingpresetstorage.moc"
