#include "telegram/TdJsonTransport.h"

#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <thread>

// TdJsonTransport runs td_receive on its own thread. stop() used to ignore a
// timed-out wait and delete the still-running QThread (which aborts), while
// the worker kept using the destroyed transport. These tests script
// td_receive to return late.
namespace
{
std::atomic_bool g_block{ false };
std::atomic_bool g_release{ false };
std::atomic_bool g_inBlockedReceive{ false };
std::atomic_int g_receiveCalls{ 0 };

const char kUpdate[] = "{\"@type\":\"updateOption\",\"name\":\"version\"}";

int fakeCreateClientId() { return 1; }
void fakeSend(int, const char*) {}
const char* fakeExecute(const char*) { return nullptr; }
const char* fakeReceive(double)
{
    ++g_receiveCalls;
    if (g_block.load()) {
        g_inBlockedReceive = true;
        // A TDLib call that does not come back in time.
        for (int i = 0; i < 1000 && !g_release.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_inBlockedReceive = false;
        return kUpdate;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return kUpdate;
}

telegram::TdJsonApi fakeApi()
{
    telegram::TdJsonApi api;
    api.createClientId = &fakeCreateClientId;
    api.send = &fakeSend;
    api.receive = &fakeReceive;
    api.execute = &fakeExecute;
    return api;
}
}

class TestTdJsonTransport : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        g_block = false;
        g_release = false;
        g_inBlockedReceive = false;
        g_receiveCalls = 0;
    }

    void deliversUpdatesOnTheOwnerThread()
    {
        telegram::TdJsonTransport transport(fakeApi());
        QThread* const owner = QThread::currentThread();
        bool onOwner = true;
        int received = 0;
        connect(&transport, &telegram::TdTransport::received, this, [&] {
            onOwner = onOwner && QThread::currentThread() == owner;
            ++received;
        });
        QVERIFY(transport.start());
        QTRY_VERIFY(received >= 2);
        QVERIFY(onOwner);
        transport.stop();
        transport.stop();   // idempotent
    }

    void aLateReceiveNeverOutlivesItsTransportUnsafely()
    {
        int received = 0;
        {
            auto transport = std::make_unique<telegram::TdJsonTransport>(fakeApi());
            connect(transport.get(), &telegram::TdTransport::received, this, [&] { ++received; });
            QVERIFY(transport->start());
            QTRY_VERIFY(received >= 1);
            g_block = true;
            QTRY_VERIFY(g_inBlockedReceive.load());
            // stop() gives up after its bounded wait. It must neither delete
            // the running thread nor leave it pointing at this transport.
            transport.reset();
        }
        const int receivedAtDestruction = received;
        // Now the blocked call returns an update for a transport that is gone.
        g_block = false;
        g_release = true;
        QTRY_VERIFY(!g_inBlockedReceive.load());
        const int calls = g_receiveCalls.load();
        QTest::qWait(800);   // the worker exits instead of receiving again
        QCOMPARE(g_receiveCalls.load(), calls);
        QCOMPARE(received, receivedAtDestruction);
    }
};

QTEST_GUILESS_MAIN(TestTdJsonTransport)
#include "tst_tdjsontransport.moc"
