#include "telegram/TdRuntime.h"
#include "telegram/TdRuntimePin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using telegram::TdRuntime;

// t22: the optional TDLib runtime is only ever loaded when it is the pinned
// build. The fake DLLs stand in for tdjson.dll; the loader must refuse every
// wrong shape without running any of its code where it can (hash first).
class TestTdRuntime : public QObject
{
    Q_OBJECT

    static QString fake(const char* name)
    {
        return QDir(QCoreApplication::applicationDirPath())
            .filePath(QString::fromLatin1(name) + QStringLiteral(".dll"));
    }

private slots:
    void pinIsWellFormed()
    {
        QCOMPARE(telegram::tdpin::kCommit.size(), 40);
        QVERIFY(!telegram::tdpin::kVersion.isEmpty());
        // Empty only until the first recorded build; when set it is a SHA-256.
        const int n = telegram::tdpin::kRuntimeSha256.size();
        QVERIFY(n == 0 || n == 64);
    }

    void unpinnedNeverLoads()
    {
        TdRuntime rt(fake("fake_tdjson_ok"), QString(), QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::Unpinned);
        QVERIFY(!rt.isReady());
        QVERIFY(!rt.api().isValid());
    }

    void missingRuntimeIsNotInstalled()
    {
        TdRuntime rt(QStringLiteral("C:/no/such/tdjson.dll"), QString(64, QLatin1Char('a')),
                     QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::NotInstalled);
    }

    void wrongHashIsRefusedBeforeLoading()
    {
        TdRuntime rt(fake("fake_tdjson_ok"), QString(64, QLatin1Char('0')), QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::HashMismatch);
        QVERIFY(!rt.api().isValid());
    }

    void pinnedBuildLoadsAndReportsVersion()
    {
        const QString hash = TdRuntime::sha256OfFile(fake("fake_tdjson_ok"));
        QCOMPARE(hash.size(), 64);
        TdRuntime rt(fake("fake_tdjson_ok"), hash.toUpper(), QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::Ready);
        QVERIFY(rt.isReady());
        QVERIFY(rt.api().isValid());
        QCOMPARE(rt.reportedVersion(), QStringLiteral("1.8.67"));
        QCOMPARE(rt.api().createClientId(), 1);
        // Idempotent.
        QCOMPARE(rt.load(), TdRuntime::Status::Ready);
    }

    void wrongVersionIsIncompatible()
    {
        const QString hash = TdRuntime::sha256OfFile(fake("fake_tdjson_oldver"));
        TdRuntime rt(fake("fake_tdjson_oldver"), hash, QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::Incompatible);
        QVERIFY(!rt.api().isValid());
    }

    void missingEntryPointIsIncompatible()
    {
        const QString hash = TdRuntime::sha256OfFile(fake("fake_tdjson_partial"));
        TdRuntime rt(fake("fake_tdjson_partial"), hash, QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::Incompatible);
    }

    void notALibraryFailsToLoad()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("tdjson.dll"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not a dll");
        f.close();
        TdRuntime rt(path, TdRuntime::sha256OfFile(path), QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::LoadFailed);
    }

    void failureIsRememberedNotRetried()
    {
        TdRuntime rt(QStringLiteral("C:/no/such/tdjson.dll"), QString(64, QLatin1Char('a')),
                     QStringLiteral("1.8.67"));
        QCOMPARE(rt.load(), TdRuntime::Status::NotInstalled);
        QCOMPARE(rt.load(), TdRuntime::Status::NotInstalled);
        QCOMPARE(TdRuntime::statusCode(rt.status()), QStringLiteral("not_installed"));
    }

    void productionLocatorUsesPinnedRelativePath()
    {
        auto rt = TdRuntime::forInstalledApp(QStringLiteral("C:/GameHQ"));
        QVERIFY(rt->dllPath().endsWith(QStringLiteral("runtime/tdlib/tdjson.dll")));
        QCOMPARE(rt->status(), TdRuntime::Status::NotLoaded);   // nothing loaded until asked
    }
};

QTEST_MAIN(TestTdRuntime)
#include "tst_tdruntime.moc"
