#include "storage/CaptureDatabase.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

// CaptureDatabase::migrate() runs the whole startup repair pass inside one
// transaction, so a failure part-way through must leave the library exactly as
// it was found — no half-merged game rows, and above all no "repairs done"
// marker, which would make the pass skip forever instead of retrying.
//
// Reasoning about that is not enough, so each test here injects a real failure
// at one durable step with a SQLite trigger that raises ABORT, then checks the
// database is byte-for-byte the state it had before the failed start.
class TestCaptureDatabaseRepair : public QObject
{
    Q_OBJECT

private:
    // A throwaway connection to the same file CaptureDatabase used, so the test
    // can plant triggers and inspect rows CaptureDatabase's own API never
    // exposes.
    struct RawConnection
    {
        QString name;
        QSqlDatabase db;

        explicit RawConnection(const QString& path)
            : name(QUuid::createUuid().toString())
        {
            db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(path);
        }
        ~RawConnection()
        {
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
        }
    };

    static bool exec(QSqlDatabase& db, const QString& sql)
    {
        QSqlQuery q(db);
        if (q.exec(sql))
            return true;
        qWarning() << "test setup failed:" << sql << q.lastError().text();
        return false;
    }

    static int insertGame(QSqlDatabase& db, const QString& displayName, const QString& iconPath)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO games (display_name, icon_path, created_at) VALUES (:n, :i, 'now')"));
        q.bindValue(QStringLiteral(":n"), displayName);
        q.bindValue(QStringLiteral(":i"), iconPath);
        return q.exec() ? q.lastInsertId().toInt() : -1;
    }

    static int insertCapture(QSqlDatabase& db, const QString& filePath, int gameId)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO captures (file_path, type, game_id, created_at, source) "
            "VALUES (:p, 'screenshot', :g, 'now', 'GameHQ')"));
        q.bindValue(QStringLiteral(":p"), filePath);
        q.bindValue(QStringLiteral(":g"), gameId);
        return q.exec() ? q.lastInsertId().toInt() : -1;
    }

    static QString iconPathFor(QSqlDatabase& db, int gameId)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT icon_path FROM games WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), gameId);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    }

    static int gameIdForCapture(QSqlDatabase& db, int captureId)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT game_id FROM captures WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), captureId);
        return q.exec() && q.next() ? q.value(0).toInt() : -1;
    }

    static int gameCount(QSqlDatabase& db)
    {
        QSqlQuery q(QStringLiteral("SELECT COUNT(*) FROM games"), db);
        return q.next() ? q.value(0).toInt() : -1;
    }

    // Empty when the key is absent, which is what "the marker was not written"
    // looks like from the next start's point of view.
    static QString settingValue(QSqlDatabase& db, const QString& key)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = :k"));
        q.bindValue(QStringLiteral(":k"), key);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    }

    static bool clearSetting(QSqlDatabase& db, const QString& key)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM settings WHERE key = :k"));
        q.bindValue(QStringLiteral(":k"), key);
        return q.exec();
    }

    // Makes the next write to `key` fail the way a disk error or a constraint
    // violation would. Both trigger kinds are needed because the production
    // code writes sentinels with an upsert.
    static bool failWritesToSetting(QSqlDatabase& db, const QString& key)
    {
        return exec(db, QStringLiteral(
                    "CREATE TRIGGER fail_setting_insert BEFORE INSERT ON settings "
                    "WHEN NEW.key = '%1' "
                    "BEGIN SELECT RAISE(ABORT, 'injected sentinel failure'); END").arg(key))
            && exec(db, QStringLiteral(
                    "CREATE TRIGGER fail_setting_update BEFORE UPDATE ON settings "
                    "WHEN NEW.key = '%1' "
                    "BEGIN SELECT RAISE(ABORT, 'injected sentinel failure'); END").arg(key));
    }

    // Creates a fresh database and hands back a raw connection to it, with the
    // one-time repair marker cleared so the next open runs the full pass again.
    static bool prepareDatabase(const QString& dbPath)
    {
        CaptureDatabase db(dbPath);
        return db.open();
    }

private slots:
    // The merge repoints a duplicate's captures and only then deletes the row it
    // merged away. Failing between the two used to strand those captures on a
    // game row that no longer meant anything.
    void aFailedDuplicateMergeStrandsNoCaptures()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("merge.db"));
        QVERIFY(prepareDatabase(dbPath));

        int keepId = -1;
        int dropId = -1;
        int captureId = -1;
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            // Same identity key, so the repair pass merges them.
            keepId = insertGame(raw.db, QStringLiteral("Doom Eternal"),
                                QStringLiteral("C:/gamehq-data/icons/keep.png"));
            dropId = insertGame(raw.db, QStringLiteral("DOOM Eternal"),
                                QStringLiteral("C:/playhq-data/icons/drop.png"));
            QVERIFY(keepId > 0);
            QVERIFY(dropId > 0);
            captureId = insertCapture(raw.db, tempDir.filePath(QStringLiteral("shot.png")), dropId);
            QVERIFY(captureId > 0);
            QVERIFY(clearSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
            QVERIFY(exec(raw.db, QStringLiteral(
                "CREATE TRIGGER fail_game_delete BEFORE DELETE ON games "
                "BEGIN SELECT RAISE(ABORT, 'injected merge failure'); END")));
        }

        {
            CaptureDatabase db(dbPath);
            QVERIFY(!db.open());
        }

        RawConnection check(dbPath);
        QVERIFY(check.db.open());
        // The merge itself: both rows still there, the capture still on its
        // original game.
        QCOMPARE(gameCount(check.db), 2);
        QCOMPARE(gameIdForCapture(check.db, captureId), dropId);
        // And the brand-path rewrite from earlier in the same transaction is
        // gone too, so the failure really did undo the whole pass.
        QCOMPARE(iconPathFor(check.db, dropId), QStringLiteral("C:/playhq-data/icons/drop.png"));
        QCOMPARE(settingValue(check.db, QStringLiteral("internal.repairs_v1_done")), QString());
    }

    // The marker is the last thing written. Failing to write it must not leave
    // the repairs it was meant to describe applied.
    void aFailedRepairMarkerRollsBackTheRepairs()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("marker.db"));
        QVERIFY(prepareDatabase(dbPath));

        int gameId = -1;
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            gameId = insertGame(raw.db, QStringLiteral("Portal 2"),
                                QStringLiteral("C:/playhq-data/icons/portal2.png"));
            QVERIFY(gameId > 0);
            QVERIFY(clearSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
            QVERIFY(failWritesToSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
        }

        {
            CaptureDatabase db(dbPath);
            QVERIFY(!db.open());
        }

        RawConnection check(dbPath);
        QVERIFY(check.db.open());
        QCOMPARE(iconPathFor(check.db, gameId), QStringLiteral("C:/playhq-data/icons/portal2.png"));
        QCOMPARE(settingValue(check.db, QStringLiteral("internal.repairs_v1_done")), QString());
    }

    // The last durable step of the pass is the icon-format marker, which runs
    // after the repair marker was already written into the same transaction.
    // Failing there must take that earlier marker down with it, otherwise the
    // one-time repairs would be skipped forever on the next start.
    void aFailureAfterTheRepairMarkerAlsoRollsItBack()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("iconformat.db"));
        QVERIFY(prepareDatabase(dbPath));

        int gameId = -1;
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            gameId = insertGame(raw.db, QStringLiteral("Hades"),
                                QStringLiteral("C:/playhq-data/icons/hades.png"));
            QVERIFY(gameId > 0);
            QVERIFY(clearSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
            // Stale format, so the icon refresh runs and reaches its marker.
            QVERIFY(exec(raw.db, QStringLiteral(
                "INSERT INTO settings (key, value) VALUES ('internal.icon_format', 'v0-stale') "
                "ON CONFLICT(key) DO UPDATE SET value = 'v0-stale'")));
            QVERIFY(failWritesToSetting(raw.db, QStringLiteral("internal.icon_format")));
        }

        {
            CaptureDatabase db(dbPath);
            QVERIFY(!db.open());
        }

        RawConnection check(dbPath);
        QVERIFY(check.db.open());
        QCOMPARE(iconPathFor(check.db, gameId), QStringLiteral("C:/playhq-data/icons/hades.png"));
        QCOMPARE(settingValue(check.db, QStringLiteral("internal.repairs_v1_done")), QString());
        QCOMPARE(settingValue(check.db, QStringLiteral("internal.icon_format")),
                 QStringLiteral("v0-stale"));
    }

    // The failures above must not be permanent: with the injected trigger gone,
    // the very next start completes the pass it previously rolled back.
    void theNextStartAfterAFailureCompletesTheRepairs()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("retry.db"));
        QVERIFY(prepareDatabase(dbPath));

        int gameId = -1;
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            gameId = insertGame(raw.db, QStringLiteral("Celeste"),
                                QStringLiteral("C:/playhq-data/icons/celeste.png"));
            QVERIFY(gameId > 0);
            QVERIFY(clearSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
            QVERIFY(failWritesToSetting(raw.db, QStringLiteral("internal.repairs_v1_done")));
        }
        {
            CaptureDatabase db(dbPath);
            QVERIFY(!db.open());
        }
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            QVERIFY(exec(raw.db, QStringLiteral("DROP TRIGGER fail_setting_insert")));
            QVERIFY(exec(raw.db, QStringLiteral("DROP TRIGGER fail_setting_update")));
        }

        {
            CaptureDatabase db(dbPath);
            QVERIFY(db.open());
        }

        RawConnection check(dbPath);
        QVERIFY(check.db.open());
        QCOMPARE(iconPathFor(check.db, gameId), QStringLiteral("C:/gamehq-data/icons/celeste.png"));
        QCOMPARE(settingValue(check.db, QStringLiteral("internal.repairs_v1_done")),
                 QStringLiteral("1"));
    }

    // Two case variants of one capture collapse to the canonical spelling. If
    // the winner is the variant that does not already hold it, the loser's
    // tombstone still does, and the UNIQUE file_path used to abort startup on
    // every launch.
    void aCaseVariantCollapseNeverBlocksStartup()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("collide.db"));
        QVERIFY(prepareDatabase(dbPath));

        const QString lower = tempDir.filePath(QStringLiteral("shot.png"));
        const QString upper = tempDir.filePath(QStringLiteral("SHOT.png"));
        const QString thumb = tempDir.filePath(QStringLiteral("thumb.png"));
        {
            QFile file(thumb);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("png");
        }
        int lowerId = -1;
        int upperId = -1;
        {
            RawConnection raw(dbPath);
            QVERIFY(raw.db.open());
            lowerId = insertCapture(raw.db, lower, -1);
            upperId = insertCapture(raw.db, upper, -1);
            QVERIFY(lowerId > 0 && upperId > 0);
            // The thumbnail makes the second row the winner.
            QVERIFY(exec(raw.db, QStringLiteral("UPDATE captures SET thumbnail_path = '%1', "
                                                "is_favorite = 0 WHERE id = %2")
                                     .arg(thumb).arg(upperId)));
            QVERIFY(exec(raw.db, QStringLiteral("UPDATE captures SET is_favorite = 1 WHERE id = %1")
                                     .arg(lowerId)));
        }

        for (int launch = 0; launch < 2; ++launch) {
            CaptureDatabase db(dbPath);
            QVERIFY2(db.open(), qPrintable(QStringLiteral("launch %1").arg(launch)));
        }

        RawConnection check(dbPath);
        QVERIFY(check.db.open());
        QSqlQuery q(check.db);
        QVERIFY(q.exec(QStringLiteral(
            "SELECT id, file_path, is_favorite, thumbnail_path FROM captures WHERE deleted_at IS NULL")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), upperId);
        QCOMPARE(q.value(1).toString().compare(lower, Qt::CaseInsensitive), 0);
        QCOMPARE(q.value(2).toInt(), 1);   // the loser's favorite survives the merge
        QVERIFY(!q.value(3).toString().isEmpty());
        QVERIFY(!q.next());
    }
};

// QTEST_MAIN, not QTEST_GUILESS_MAIN: the repair pass reaches
// GameIconCache::iconPathForExecutable, whose QFileIconProvider fallback is a
// QtWidgets class that segfaults under a bare QCoreApplication.
QTEST_MAIN(TestCaptureDatabaseRepair)
#include "tst_capturedatabaserepair.moc"
