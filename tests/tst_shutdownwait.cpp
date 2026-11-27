#include "core/ShutdownWait.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTest>
#include <QThread>

#include <atomic>
#include <thread>

// t41/A11: a shutdown wait never abandons the work it waits for, and says so
// when that work outlives the grace period.
class TestShutdownWait : public QObject
{
    Q_OBJECT

private slots:
    void quickWorkFinishesSilentlyInsideTheGrace()
    {
        std::atomic_bool done{false};
        std::thread worker([&] { QThread::msleep(30); done = true; });
        const bool inGrace = ShutdownWait::untilDone(
            "fixture", [&](int ms) {
                QElapsedTimer t;
                t.start();
                while (!done && t.elapsed() < ms)
                    QThread::msleep(1);
                return done.load();
            }, 500);
        worker.join();
        QVERIFY(inGrace);
        QVERIFY(done);
    }

    void slowWorkIsReportedAndStillAwaited()
    {
        std::atomic_bool done{false};
        std::thread worker([&] { QThread::msleep(700); done = true; });
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Shutdown: slow fixture still running after \\d+ ms"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Shutdown: slow fixture finished after \\d+ ms"));
        QElapsedTimer total;
        total.start();
        const bool inGrace = ShutdownWait::untilDone(
            "slow fixture", [&](int ms) {
                QElapsedTimer t;
                t.start();
                while (!done && t.elapsed() < ms)
                    QThread::msleep(1);
                return done.load();
            }, 200);
        // Returned only after the work finished, never at the deadline.
        QVERIFY(done);
        QVERIFY(!inGrace);
        QVERIFY(total.elapsed() >= 650);
        worker.join();
    }
};

QTEST_GUILESS_MAIN(TestShutdownWait)
#include "tst_shutdownwait.moc"
