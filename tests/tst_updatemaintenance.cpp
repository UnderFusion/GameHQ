#include <QtTest>

#include "core/UpdateMaintenance.h"

#include <QTemporaryDir>

#include <fstream>
#include <windows.h>

class UpdateMaintenanceTest : public QObject
{
    Q_OBJECT
private slots:
    void markerLifecycleAndStaleRecovery();
    void startupRecovery_data();
    void startupRecovery();
    void completedCleanupFailure_data();
    void completedCleanupFailure();
    void refusesIncompleteCleanup();
};

void UpdateMaintenanceTest::markerLifecycleAndStaleRecovery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    const auto now = std::filesystem::file_time_type::clock::now();
    QCOMPARE(maintenance::inspect(root, false, now).state,
             maintenance::State::Inactive);

    std::string error;
    QVERIFY2(maintenance::begin(root, error), error.c_str());
    QCOMPARE(maintenance::inspect(root, false, now).state,
             maintenance::State::Active);

    const auto marker = root / L".update" / L"maintenance.lock";
    std::filesystem::last_write_time(marker, now - std::chrono::minutes(6));
    QCOMPARE(maintenance::inspect(root, true, now).state,
             maintenance::State::Active);
    QCOMPARE(maintenance::inspect(root, false, now).state,
             maintenance::State::StaleRecovery);

    std::ofstream(root / L".update" / L"transaction.phase") << "healthy\n";
    QCOMPARE(maintenance::inspect(root, true, now).state,
             maintenance::State::Active);
    QCOMPARE(maintenance::inspect(root, false, now).state,
             maintenance::State::Inactive);
    maintenance::finish(root);
    QVERIFY(!std::filesystem::exists(marker));
}

void UpdateMaintenanceTest::startupRecovery_data()
{
    QTest::addColumn<QByteArray>("phase");
    QTest::addColumn<bool>("journal");
    QTest::addColumn<bool>("expected");
    QTest::newRow("clean-install") << QByteArray() << false << false;
    QTest::newRow("orphan-journal") << QByteArray() << true << true;
    for (const QByteArray phase : {QByteArray("healthy"), QByteArray("rolled_back")}) {
        QTest::newRow((phase + "-journal").constData()) << phase << true << false;
        QTest::newRow((phase + "-no-journal").constData()) << phase << false << false;
    }
    for (const QByteArray phase : {QByteArray("staged"), QByteArray("data_snapshotted"),
                                   QByteArray("swapped"), QByteArray("validating"),
                                   QByteArray("unknown")}) {
        QTest::newRow((phase + "-journal").constData()) << phase << true << true;
        QTest::newRow((phase + "-no-journal").constData()) << phase << false << true;
    }
}

void UpdateMaintenanceTest::startupRecovery()
{
    QFETCH(QByteArray, phase);
    QFETCH(bool, journal);
    QFETCH(bool, expected);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    std::filesystem::create_directories(root / L".update");
    if (!phase.isEmpty())
        std::ofstream(root / L".update" / L"transaction.phase", std::ios::binary)
            << phase.constData() << "\r\n";
    if (journal)
        std::ofstream(root / L".update" / L"swap.manifest") << "retained journal\n";
    QCOMPARE(maintenance::recoveryRequired(root), expected);
    // Deciding to start must leave rollback evidence untouched.
    QCOMPARE(std::filesystem::exists(root / L".update" / L"swap.manifest"), journal);
}

void UpdateMaintenanceTest::completedCleanupFailure_data()
{
    QTest::addColumn<QString>("lockedRelative");
    QTest::addColumn<QByteArray>("phase");
    for (const QByteArray phase : {QByteArray("healthy"), QByteArray("rolled_back")}) {
        for (const QString relative : {QStringLiteral("backup/sentinel"),
                                        QStringLiteral("swap.manifest"),
                                        QStringLiteral("healthy.token"),
                                        QStringLiteral("transaction.json"),
                                        QStringLiteral("transaction.phase")}) {
            QTest::newRow((phase + '-' + relative.toUtf8()).constData()) << relative << phase;
        }
    }
}

void UpdateMaintenanceTest::completedCleanupFailure()
{
    QFETCH(QString, lockedRelative);
    QFETCH(QByteArray, phase);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    const auto update = root / L".update";
    for (const wchar_t *name : {L"backup", L"data-snapshot", L"staging", L"failed-new", L"downloads"}) {
        std::filesystem::create_directories(update / name);
        std::ofstream(update / name / L"sentinel") << "diagnostic data";
    }
    for (const wchar_t *name : {L"swap.manifest", L"healthy.token", L"transaction.json", L"updater.log"})
        std::ofstream(update / name) << "diagnostic evidence";
    std::ofstream(update / L"transaction.phase") << phase.constData() << '\n';
    const auto lockedPath = update / lockedRelative.toStdWString();
    HANDLE locked = CreateFileW(lockedPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(locked != INVALID_HANDLE_VALUE);
    std::string error;
    const bool cleaned = maintenance::cleanupCompleted(root, error);
    CloseHandle(locked);
    QVERIFY(!cleaned);
    QVERIFY(!error.empty());
    QVERIFY(std::filesystem::exists(update / L"transaction.phase"));
    QVERIFY(std::filesystem::exists(lockedPath));
    // The next ordinary startup remains possible after any partial failure.
    QVERIFY(!maintenance::recoveryRequired(root));
    QVERIFY2(maintenance::cleanupCompleted(root, error), error.c_str());
    QVERIFY(!std::filesystem::exists(update / L"transaction.phase"));
    QVERIFY(!std::filesystem::exists(update / L"swap.manifest"));
    QVERIFY(!std::filesystem::exists(update / L"backup"));
    QVERIFY(std::filesystem::exists(update / L"updater.log"));
    QVERIFY(!maintenance::recoveryRequired(root));
}

void UpdateMaintenanceTest::refusesIncompleteCleanup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    const auto update = root / L".update";
    std::filesystem::create_directories(update / L"backup");
    std::ofstream(update / L"transaction.phase") << "validating\n";
    std::ofstream(update / L"swap.manifest") << "journal";
    std::ofstream(update / L"backup" / L"sentinel") << "rollback data";
    std::string error;
    QVERIFY(!maintenance::cleanupCompleted(root, error));
    QVERIFY(std::filesystem::exists(update / L"backup" / L"sentinel"));
    QVERIFY(maintenance::recoveryRequired(root));
}

QTEST_APPLESS_MAIN(UpdateMaintenanceTest)
#include "tst_updatemaintenance.moc"
