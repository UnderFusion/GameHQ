#include "overlay/ForegroundAcquirer.h"
#include "overlay/ForegroundApi.h"

#include <QSignalSpy>
#include <QtTest>

namespace
{
// Refuses to move the foreground until `succeedOnAttempt` (0 = never). Real
// Windows foreground denial cannot be forced deterministically; this can.
class FakeForegroundApi final : public ForegroundApi
{
public:
    void* current = nullptr;
    int succeedOnAttempt = 1;
    int attempts = 0;

    void* foregroundWindow() override { return current; }
    bool forceForeground(void* target) override
    {
        ++attempts;
        if (succeedOnAttempt > 0 && attempts >= succeedOnAttempt) {
            current = target;
            return true;
        }
        return false;
    }
};

void* hwnd(quintptr id) { return reinterpret_cast<void*>(id); }
} // namespace

class ForegroundAcquirerTest : public QObject
{
    Q_OBJECT

private slots:
    void firstAttemptSuccessFinishesSynchronously()
    {
        auto* api = new FakeForegroundApi;
        ForegroundAcquirer acquirer(api);
        QSignalSpy spy(&acquirer, &ForegroundAcquirer::finished);

        acquirer.acquire(hwnd(0x10), QStringLiteral("overlay show"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(2).toBool(), true);
        QCOMPARE(spy.first().at(3).toInt(), 1);
        QCOMPARE(api->attempts, 1);
        QVERIFY(acquirer.lastAcquired());
    }

    void persistentDenialStopsAfterTwoRetriesAndTellsTheTruth()
    {
        auto* api = new FakeForegroundApi;
        api->succeedOnAttempt = 0;   // the shell never yields
        ForegroundAcquirer acquirer(api);
        QSignalSpy spy(&acquirer, &ForegroundAcquirer::finished);

        acquirer.acquire(hwnd(0x11), QStringLiteral("overlay show"));
        QCOMPARE(spy.count(), 0);    // still retrying, not busy-waiting
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
        QCOMPARE(api->attempts, ForegroundAcquirer::kMaxAttempts);
        QCOMPARE(spy.first().at(2).toBool(), false);
        QCOMPARE(spy.first().at(3).toInt(), ForegroundAcquirer::kMaxAttempts);
        QVERIFY(!acquirer.lastAcquired());

        // The budget is spent: no further attempts ever happen.
        QTest::qWait(300);
        QCOMPARE(api->attempts, ForegroundAcquirer::kMaxAttempts);
    }

    void transientDenialSucceedsOnRetry()
    {
        auto* api = new FakeForegroundApi;
        api->succeedOnAttempt = 2;
        ForegroundAcquirer acquirer(api);
        QSignalSpy spy(&acquirer, &ForegroundAcquirer::finished);

        acquirer.acquire(hwnd(0x12), QStringLiteral("overlay show"));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
        QCOMPARE(api->attempts, 2);
        QCOMPARE(spy.first().at(2).toBool(), true);
        QVERIFY(acquirer.lastAcquired());
    }

    void newAcquireSupersedesPendingRetries()
    {
        auto* api = new FakeForegroundApi;
        api->succeedOnAttempt = 0;
        ForegroundAcquirer acquirer(api);
        QSignalSpy spy(&acquirer, &ForegroundAcquirer::finished);

        acquirer.acquire(hwnd(0x13), QStringLiteral("overlay show"));
        acquirer.acquire(hwnd(0x14), QStringLiteral("overlay hide"));

        // Only the second acquisition reports; the first chain was orphaned
        // mid-retry instead of finishing with a stale verdict.
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);
        QTest::qWait(300);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("overlay hide"));
    }

    void cancelPreventsClosedOverlayFromReacquiringFocus()
    {
        auto* api = new FakeForegroundApi;
        api->succeedOnAttempt = 2;
        api->current = hwnd(0x20);
        ForegroundAcquirer acquirer(api);
        QSignalSpy spy(&acquirer, &ForegroundAcquirer::finished);

        acquirer.acquire(hwnd(0x21), QStringLiteral("overlay show"));
        acquirer.cancel();
        QTest::qWait(300);
        QCOMPARE(api->attempts, 1);
        QCOMPARE(api->current, hwnd(0x20));
        QCOMPARE(spy.count(), 0);

        acquirer.acquire(hwnd(0x22), QStringLiteral("overlay show"));
        QCOMPARE(api->current, hwnd(0x22));
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_GUILESS_MAIN(ForegroundAcquirerTest)
#include "tst_foregroundacquirer.moc"
