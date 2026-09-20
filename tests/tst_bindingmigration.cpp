// Schema v3 -> v4: tap counts get a column of their own.
//
// The interesting part is not the ALTER — it is that every assignment a user
// already made keeps meaning the same thing afterwards. A `double_tap` row must
// come back as "tap, twice" with its scope, device profile and cleared-slot
// metadata untouched, and a row that is already broken must not take the
// migration (or the app) down with it.
//
// The fixture is built by letting CaptureDatabase create a current database and
// then rewriting binding_overrides back to its v3 shape. That is a real v3
// database without duplicating the whole v1 schema here, and it fails loudly if
// the v3 column list ever drifts from what this test assumes.

#include "input/BindingResolver.h"
#include "input/ExtraButtonCatalog.h"
#include "storage/CaptureDatabase.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

namespace {

const QString kV3Table = QStringLiteral(R"(CREATE TABLE binding_overrides (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    device_group   TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
    device_profile TEXT NOT NULL DEFAULT '',
    action_id      TEXT NOT NULL,
    slot           INTEGER NOT NULL DEFAULT 1 CHECK(slot IN (1,2)),
    trigger_code   TEXT,
    activation     TEXT NOT NULL DEFAULT 'press' CHECK(activation IN ('press','tap','hold','double_tap')),
    hold_ms        INTEGER,
    unbound        INTEGER NOT NULL DEFAULT 0 CHECK(unbound IN (0,1))))");

// The gesture a persisted row means, read through the same model the resolver
// uses. Kept here rather than on BindingOverrideRow so storage does not have to
// know about the input layer.
GestureSpec gestureOf(const BindingOverrideRow& row)
{
    const auto parsed = GestureSpec::parse(row.activation, row.tapCount, row.holdMs);
    return parsed.ok ? parsed.gesture : GestureSpec{GestureSpec::Kind::Press, -1, -1};
}

ModernInput::GameInputButtonDescriptor extraButton(const char* label)
{
    return {-1, QString::fromLatin1(label),
            ModernInput::GameInputButtonClassification::Extra};
}

struct LegacyRow {
    const char* group;
    const char* profile;
    const char* action;
    int slot;
    const char* trigger;
    const char* activation;
    QVariant holdMs;
    int unbound;
};

} // namespace

class BindingMigrationTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_path;

    // Creates a database at schema v3 holding `rows`.
    bool buildV3Fixture(const QVector<LegacyRow>& rows)
    {
        {
            CaptureDatabase current(m_path);
            if (!current.open())
                return false;
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));

        bool ok = true;
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("fixture"));
            raw.setDatabaseName(m_path);
            if (!raw.open())
                return false;
            const QStringList reshape = {
                QStringLiteral("DROP INDEX IF EXISTS idx_binding_overrides_scope"),
                QStringLiteral("DROP TABLE binding_overrides"),
                kV3Table,
                QStringLiteral("CREATE UNIQUE INDEX idx_binding_overrides_scope "
                               "ON binding_overrides(device_group, device_profile, action_id, slot)"),
                QStringLiteral("PRAGMA user_version = 3"),
            };
            for (const QString& sql : reshape) {
                QSqlQuery q(raw);
                if (!q.exec(sql)) {
                    qWarning() << "fixture failed:" << sql << q.lastError().text();
                    ok = false;
                }
            }
            for (const LegacyRow& row : rows) {
                QSqlQuery q(raw);
                q.prepare(QStringLiteral(
                    "INSERT INTO binding_overrides (device_group, device_profile, action_id, "
                    "slot, trigger_code, activation, hold_ms, unbound) VALUES "
                    "(:g, :p, :a, :s, :t, :act, :hold, :u)"));
                q.bindValue(QStringLiteral(":g"), QString::fromLatin1(row.group));
                q.bindValue(QStringLiteral(":p"), QString::fromLatin1(row.profile));
                q.bindValue(QStringLiteral(":a"), QString::fromLatin1(row.action));
                q.bindValue(QStringLiteral(":s"), row.slot);
                q.bindValue(QStringLiteral(":t"), row.trigger[0] == '\0'
                                                      ? QVariant()
                                                      : QVariant(QString::fromLatin1(row.trigger)));
                q.bindValue(QStringLiteral(":act"), QString::fromLatin1(row.activation));
                q.bindValue(QStringLiteral(":hold"), row.holdMs);
                q.bindValue(QStringLiteral(":u"), row.unbound);
                if (!q.exec()) {
                    qWarning() << "fixture row failed:" << q.lastError().text();
                    ok = false;
                }
            }
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));
        return ok;
    }

    static BindingOverrideRow find(const QVector<BindingOverrideRow>& rows,
                                   const QString& actionId, int slot)
    {
        for (const BindingOverrideRow& row : rows) {
            if (row.actionId == actionId && row.slot == slot)
                return row;
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("gamehq.db"));
    }

    void cleanup()
    {
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
        QFile::remove(m_path);
    }

    void everyLegacyActivationSurvivesTheUpgrade()
    {
        QVERIFY(buildV3Fixture({
            {"controller", "", "global.toggle_overlay", 1, "gamepad.guide", "press", {}, 0},
            {"controller", "", "global.screenshot", 1, "gamepad.capture", "tap", {}, 0},
            {"controller", "", "global.toggle_overlay", 2, "gamepad.capture", "double_tap", {}, 0},
            {"controller", "", "global.save_replay", 1, "gamepad.capture", "hold", 3000, 0},
            // Per-device scope and a cleared slot: both must come through with
            // their gesture, not be flattened into a group-wide press.
            {"controller", "054C:0CE6", "desktop.favorite", 2, "gamepad.button.17", "tap", {}, 0},
            {"controller", "", "desktop.bulk_toggle", 1, "", "double_tap", {}, 1},
            {"keyboard", "", "global.screenshot", 2, "Ctrl+Alt+P", "press", {}, 0},
        }));

        CaptureDatabase database(m_path);
        QVERIFY(database.open());
        QCOMPARE(database.schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);

        const auto rows = database.listBindingOverrides();
        QCOMPARE(rows.size(), 7);

        // v7 rewrites the legacy Guide-press overlay toggle to a single tap so
        // the new default Guide hold (Show / Hide GameHQ) can arm — a press
        // fires on the down edge and cancels the recognizer before the hold.
        const auto press = find(rows, QStringLiteral("global.toggle_overlay"), 1);
        QCOMPARE(gestureOf(press), GestureSpec::tap(1));

        const auto tap = find(rows, QStringLiteral("global.screenshot"), 1);
        QCOMPARE(gestureOf(tap), GestureSpec::tap(1));

        // The point of the migration: the count leaves the activation string.
        const auto doubleTap = find(rows, QStringLiteral("global.toggle_overlay"), 2);
        QCOMPARE(doubleTap.activation, QStringLiteral("tap"));
        QCOMPARE(gestureOf(doubleTap), GestureSpec::tap(2));

        const auto hold = find(rows, QStringLiteral("global.save_replay"), 1);
        QCOMPARE(gestureOf(hold), GestureSpec::hold(3000));

        const auto scoped = find(rows, QStringLiteral("desktop.favorite"), 2);
        QCOMPARE(scoped.deviceProfile, QStringLiteral("054C:0CE6"));
        QCOMPARE(gestureOf(scoped), GestureSpec::tap(1));

        const auto cleared = find(rows, QStringLiteral("desktop.bulk_toggle"), 1);
        QVERIFY(cleared.unbound);
        QVERIFY(cleared.triggerCode.isEmpty());
        QCOMPARE(gestureOf(cleared), GestureSpec::tap(2));

        const auto keyboard = find(rows, QStringLiteral("global.screenshot"), 2);
        QCOMPARE(keyboard.deviceGroup, QStringLiteral("keyboard"));
        QCOMPARE(keyboard.triggerCode, QStringLiteral("Ctrl+Alt+P"));
        QCOMPARE(gestureOf(keyboard), GestureSpec::press());
    }

    void migratedRowsStillResolveAndBrokenOnesAreSkipped()
    {
        QVERIFY(buildV3Fixture({
            {"controller", "", "global.toggle_overlay", 2, "gamepad.capture", "double_tap", {}, 0},
            // Corrupted outside the app: a press cannot carry a hold duration.
            {"controller", "", "desktop.favorite", 2, "gamepad.button.18", "press", 900, 0},
        }));

        CaptureDatabase database(m_path);
        QVERIFY(database.open());
        BindingResolver resolver(&database);
        resolver.reload();

        const auto effective = resolver.effectiveBindings(QStringLiteral("controller"));
        bool sawOverlay = false;
        for (const auto& binding : effective) {
            if (binding.actionId == QLatin1String("global.toggle_overlay") && binding.slot == 2) {
                sawOverlay = true;
                QCOMPARE(binding.gesture(), GestureSpec::tap(2));
            }
            QVERIFY2(!(binding.actionId == QLatin1String("desktop.favorite") && binding.slot == 2),
                     "the corrupted row must not become a live binding");
        }
        QVERIFY(sawOverlay);

        // The surviving assignment still answers the gesture it was saved with.
        const auto matches = resolver.matching(QStringLiteral("controller"), {},
                                               QStringLiteral("gamepad.capture"),
                                               GestureSpec::tap(2),
                                               ActionCatalog::Scope::Overlay);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches.first().actionId, QStringLiteral("global.toggle_overlay"));
    }

    void viewBackMigrationTouchesOnlyProvenLegacyXInputProfiles()
    {
        {
            CaptureDatabase current(m_path);
            QVERIFY(current.open());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("fixture"));
            raw.setDatabaseName(m_path);
            QVERIFY(raw.open());
            QSqlQuery insert(raw);
            QVERIFY(insert.exec(QStringLiteral(
                "INSERT INTO binding_overrides "
                "(device_group,device_profile,action_id,slot,trigger_code,activation,hold_ms,unbound,tap_count) VALUES "
                "('controller','xinput.slot0','legacy.simple',1,'gamepad.capture','tap',0,0,1),"
                "('controller','xinput.slot1','legacy.chord',1,'chord:v1:gamepad.capture>gamepad.guide','press',0,0,1),"
                "('controller','','shared.keep',1,'gamepad.capture','tap',0,0,1),"
                "('controller','054C:0CE6','model.keep',1,'gamepad.capture','tap',0,0,1)")));
            QVERIFY(QSqlQuery(QStringLiteral("PRAGMA user_version = 4"), raw).isActive());
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));

        CaptureDatabase migrated(m_path);
        QVERIFY(migrated.open());
        QCOMPARE(migrated.schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);
        const auto rows = migrated.listBindingOverrides();
        QCOMPARE(find(rows, QStringLiteral("legacy.simple"), 1).triggerCode,
                 QStringLiteral("gamepad.view_back"));
        QCOMPARE(find(rows, QStringLiteral("legacy.chord"), 1).triggerCode,
                 QStringLiteral("chord:v1:gamepad.view_back>gamepad.guide"));
        QCOMPARE(find(rows, QStringLiteral("shared.keep"), 1).triggerCode,
                 QStringLiteral("gamepad.capture"));
        QCOMPARE(find(rows, QStringLiteral("model.keep"), 1).triggerCode,
                 QStringLiteral("gamepad.capture"));
    }

    // Schema v6 -> v7: the pre-hold Guide-press overlay override becomes a tap
    // (same meaning, hold-compatible); a Guide press the user bound to another
    // action is preserved and instead disables the new default Guide hold for
    // that profile, so both actions never fire from one press.
    void guidePressOverridesMigrateWithoutBreakingTheGuideHold()
    {
        {
            CaptureDatabase current(m_path);
            QVERIFY(current.open());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("fixture"));
            raw.setDatabaseName(m_path);
            QVERIFY(raw.open());
            QSqlQuery insert(raw);
            QVERIFY(insert.exec(QStringLiteral(
                "INSERT INTO binding_overrides "
                "(device_group,device_profile,action_id,slot,trigger_code,activation,hold_ms,unbound,tap_count) VALUES "
                // The old persisted default: overlay toggle on Guide press.
                "('controller','','global.toggle_overlay',1,'gamepad.guide','press',0,0,1),"
                // Same override saved per-device: profile must survive.
                "('controller','054C:0CE6','global.toggle_overlay',1,'gamepad.guide','press',0,0,1),"
                // A custom Guide press on another action: kept as saved.
                "('controller','AAAA:BBBB','global.screenshot',1,'gamepad.guide','press',0,0,1),"
                // A profile that already reassigned the desktop toggle: the
                // suppression insert must not overwrite the user's row.
                "('controller','CCCC:DDDD','global.save_replay',2,'gamepad.guide','press',0,0,1),"
                "('controller','CCCC:DDDD','global.toggle_desktop',1,'gamepad.menu','hold',1500,0,1)")));
            QVERIFY(QSqlQuery(QStringLiteral("PRAGMA user_version = 6"), raw).isActive());
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));

        CaptureDatabase migrated(m_path);
        QVERIFY(migrated.open());
        QCOMPARE(migrated.schemaVersion(), CaptureDatabase::kCurrentSchemaVersion);
        const auto rows = migrated.listBindingOverrides();

        // Overlay toggles became taps, shared and device-specific alike.
        const auto shared = find(rows, QStringLiteral("global.toggle_overlay"), 1);
        QCOMPARE(shared.deviceProfile, QString());
        QCOMPARE(gestureOf(shared), GestureSpec::tap(1));
        QCOMPARE(shared.triggerCode, QStringLiteral("gamepad.guide"));
        bool sawDeviceRow = false;
        for (const BindingOverrideRow& row : rows) {
            if (row.actionId == QLatin1String("global.toggle_overlay")
                && row.deviceProfile == QLatin1String("054C:0CE6")) {
                sawDeviceRow = true;
                QCOMPARE(gestureOf(row), GestureSpec::tap(1));
            }
        }
        QVERIFY(sawDeviceRow);

        // The custom press is untouched, and the default Guide hold is cleared
        // for exactly that profile.
        const auto custom = find(rows, QStringLiteral("global.screenshot"), 1);
        QCOMPARE(gestureOf(custom), GestureSpec::press());
        QCOMPARE(custom.triggerCode, QStringLiteral("gamepad.guide"));
        bool sawSuppression = false;
        for (const BindingOverrideRow& row : rows) {
            if (row.actionId == QLatin1String("global.toggle_desktop")
                && row.deviceProfile == QLatin1String("AAAA:BBBB")) {
                sawSuppression = true;
                QVERIFY(row.unbound);
                QVERIFY(row.triggerCode.isEmpty());
            }
        }
        QVERIFY(sawSuppression);

        // The user's own desktop-toggle reassignment beat the suppression row.
        for (const BindingOverrideRow& row : rows) {
            if (row.actionId == QLatin1String("global.toggle_desktop")
                && row.deviceProfile == QLatin1String("CCCC:DDDD")) {
                QVERIFY(!row.unbound);
                QCOMPARE(row.triggerCode, QStringLiteral("gamepad.menu"));
            }
        }

        // Resolved views: the shared profile keeps the default Guide hold next
        // to the migrated tap; the custom-press profile does not offer it.
        BindingResolver resolver(&migrated);
        resolver.reload();
        bool sharedHold = false;
        for (const auto& binding : resolver.effectiveBindings(QStringLiteral("controller"))) {
            if (binding.actionId == QLatin1String("global.toggle_overlay") && binding.slot == 1)
                QCOMPARE(binding.gesture(), GestureSpec::tap(1));
            if (binding.actionId == QLatin1String("global.toggle_desktop")
                && binding.triggerCode == QLatin1String("gamepad.guide"))
                sharedHold = true;
        }
        QVERIFY(sharedHold);
        for (const auto& binding : resolver.effectiveBindings(QStringLiteral("controller"),
                                                              QStringLiteral("AAAA:BBBB"))) {
            QVERIFY2(binding.actionId != QLatin1String("global.toggle_desktop"),
                     "the default Guide hold must stay suppressed for a profile "
                     "with a custom Guide press");
        }
    }

    void extraButtonLayoutChangesRequireReconfirmation()
    {
        CaptureDatabase database(m_path);
        QVERIFY(database.open());
        ModernInput::ExtraButtonCatalog catalog(&database);

        const auto first = catalog.observe(QStringLiteral("controller-a"), 3,
                                           {extraButton("P1"), extraButton("P2"),
                                            extraButton("P3")});
        QVERIFY(!first.changed);
        QVERIFY(!first.needsReconfirmation);
        QCOMPARE(first.controlIds.size(), 3);
        QVERIFY(ControlId::isCanonical(first.controlIds.at(2)));

        const auto same = catalog.observe(QStringLiteral("controller-a"), 3,
                                          {extraButton("P1"), extraButton("P2"),
                                           extraButton("P3")});
        QCOMPARE(same.signature, first.signature);
        QVERIFY(!same.changed);

        const auto changed = catalog.observe(QStringLiteral("controller-a"), 3,
                                             {extraButton("P2"), extraButton("P1"),
                                              extraButton("P3")});
        QVERIFY(changed.changed);
        QVERIFY(changed.needsReconfirmation);
        QVERIFY(changed.controlIds.at(0) != first.controlIds.at(0));
        QVERIFY(database.controllerLayout(QStringLiteral("controller-a")).needsReconfirmation);
        QVERIFY(catalog.confirm(QStringLiteral("controller-a")));
        QVERIFY(!database.controllerLayout(QStringLiteral("controller-a")).needsReconfirmation);
    }

    void aNewerSchemaIsRefusedRatherThanMisread()
    {
        {
            CaptureDatabase current(m_path);
            QVERIFY(current.open());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("fixture"));
            raw.setDatabaseName(m_path);
            QVERIFY(raw.open());
            QSqlQuery(QStringLiteral("PRAGMA user_version = %1")
                          .arg(CaptureDatabase::kCurrentSchemaVersion + 1), raw);
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));

        CaptureDatabase future(m_path);
        QVERIFY2(!future.open(), "a database from a newer build must not be opened");
    }
};

QTEST_MAIN(BindingMigrationTest)
#include "tst_bindingmigration.moc"
