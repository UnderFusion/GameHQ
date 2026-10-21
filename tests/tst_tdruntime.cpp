#include "telegram/TdRuntime.h"
#include "telegram/TdRuntimePin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

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

    // aud-b9 follow-up: a transport worker can still be inside td_receive
    // after its transport and runtime are gone. A Ready runtime pins the
    // module, so even unbalanced FreeLibrary calls cannot unmap it.
    void readyRuntimeStaysMappedAfterDestructionAndFree()
    {
        const QString path = fake("fake_tdjson_ok");
        {
            TdRuntime rt(path, TdRuntime::sha256OfFile(path), QStringLiteral("1.8.67"));
            QCOMPARE(rt.load(), TdRuntime::Status::Ready);
        }
        const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
        HMODULE module = GetModuleHandleW(native.c_str());
        QVERIFY(module);
        for (int i = 0; i < 16; ++i)
            FreeLibrary(module);
        QCOMPARE(GetModuleHandleW(native.c_str()), module);
        QVERIFY(GetProcAddress(module, "td_receive"));
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
