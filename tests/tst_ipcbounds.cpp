#include "integration/IntegrationProtocol.h"
#include "integration/LocalIntegrationServer.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <memory>

// t42/A12: both local endpoints under a burst. The servers used to readAll()
// whatever the socket had buffered (QLocalSocket buffers without limit), so a
// burst that arrived while the GUI thread was busy was copied in full and then
// rejected by the decoder's limit, even when every frame was valid; and every
// decoded frame was dispatched in the same event-loop turn.
namespace
{
QByteArray statusFrame(int i, int padding)
{
    QString error;
    const QJsonObject object{{QStringLiteral("type"), QStringLiteral("status.request")},
                             {QStringLiteral("requestId"), QStringLiteral("r%1").arg(i)},
                             {QStringLiteral("pad"), QString(padding, QLatin1Char('x'))}};
    return integration::encodeFrame(object, error);
}

// Writes `payload` from its own thread and keeps the connection open.
class Writer
{
public:
    Writer(QString name, QByteArray payload, int chunk = 0)
        : m_name(std::move(name)), m_payload(std::move(payload)), m_chunk(chunk)
    {
        m_thread.reset(QThread::create([this] { run(); }));
        m_thread->start();
    }
    ~Writer()
    {
        m_stop = true;
        m_thread->wait();
    }
    bool connected() const { return m_connected; }
    bool written() const { return m_written; }
    bool closedByServer() const { return m_closed; }

private:
    void run()
    {
        QLocalSocket socket;
        socket.connectToServer(m_name);
        if (!socket.waitForConnected(3000))
            return;
        m_connected = true;
        const int chunk = m_chunk > 0 ? m_chunk : int(m_payload.size());
        for (qsizetype at = 0; at < m_payload.size() && !m_stop; at += chunk) {
            socket.write(m_payload.mid(at, chunk));
            while (socket.bytesToWrite() > 0 && !m_stop
                   && socket.state() == QLocalSocket::ConnectedState)
                socket.waitForBytesWritten(50);
        }
        m_written = true;
        while (!m_stop && socket.state() == QLocalSocket::ConnectedState)
            socket.waitForReadyRead(20);
        m_closed = socket.state() != QLocalSocket::ConnectedState && !m_stop;
    }

    QString m_name;
    QByteArray m_payload;
    int m_chunk;
    std::unique_ptr<QThread> m_thread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_connected{false};
    std::atomic_bool m_written{false};
    std::atomic_bool m_closed{false};
};

// Counts event-loop turns so messages handled per turn can be measured.
struct TurnCounter
{
    QTimer timer;
    int turn = 0;
    TurnCounter()
    {
        timer.setInterval(0);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { ++turn; });
        timer.start();
    }
};
} // namespace

class TestIpcBounds : public QObject
{
    Q_OBJECT

private slots:
    void aValidBurstDuringAStallIsDeliveredInBoundedTurns_data()
    {
        QTest::addColumn<int>("frames");
        QTest::addColumn<int>("padding");
        QTest::addColumn<int>("chunk");
        QTest::newRow("1000 x 1 KB, one write") << 1000 << 1000 << 0;
        QTest::newRow("4000 x 64 B, one write") << 4000 << 16 << 0;
        QTest::newRow("500 x 1 KB, 7-byte chunks") << 500 << 1000 << 7;
    }

    void aValidBurstDuringAStallIsDeliveredInBoundedTurns()
    {
        QFETCH(int, frames);
        QFETCH(int, padding);
        QFETCH(int, chunk);
        const QString name = QStringLiteral("gamehq-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        LocalIntegrationServer server;
        QString error;
        QVERIFY2(server.start(error, name), qPrintable(error));
        QSignalSpy disconnected(&server, &LocalIntegrationServer::clientDisconnected);
        TurnCounter turns;
        int received = 0, lastTurn = -1, thisTurn = 0, maxPerTurn = 0;
        QString order;
        bool inOrder = true;
        connect(&server, &LocalIntegrationServer::messageReceived, this,
                [&](quint64, const integration::Message& m) {
                    if (turns.turn != lastTurn) {
                        lastTurn = turns.turn;
                        thisTurn = 0;
                    }
                    maxPerTurn = qMax(maxPerTurn, ++thisTurn);
                    inOrder = inOrder && m.requestId == QStringLiteral("r%1").arg(received);
                    ++received;
                });
        QByteArray payload;
        for (int i = 0; i < frames; ++i)
            payload += statusFrame(i, padding);
        QSignalSpy connectedSpy(&server, &LocalIntegrationServer::clientConnected);
        Writer writer(name, payload, chunk);
        QTRY_VERIFY(!connectedSpy.isEmpty());
        // The GUI thread is busy (a layout pass, a scan apply) while it writes.
        QThread::msleep(chunk ? 300 : 800);
        QTRY_VERIFY_WITH_TIMEOUT(writer.written() || !disconnected.isEmpty(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(received == frames || !disconnected.isEmpty(), 20000);
        qInfo().noquote() << QStringLiteral("%1 bytes in %2 frames: delivered %3, disconnected %4, "
                                            "max per read %5 (per counted turn %6), peak taken %7 bytes")
                                 .arg(payload.size()).arg(frames).arg(received)
                                 .arg(disconnected.size()).arg(server.peakMessagesPerRead())
                                 .arg(maxPerTurn).arg(server.peakBufferedBytes());
        QCOMPARE(disconnected.size(), 0);
        QCOMPARE(received, frames);
        QVERIFY(inOrder);
        QVERIFY(server.peakMessagesPerRead() <= LocalIntegrationServer::kMaxMessagesPerTurn);
        QVERIFY(server.peakBufferedBytes() <= integration::MaximumBufferedBytes);
    }

    // A writer far beyond any protocol limit while the GUI thread is stalled:
    // the bytes must stay in the writer (pipe backpressure), not pile up in
    // GameHQ's memory.
    void aStalledServerAppliesBackpressureToAFloodingWriter()
    {
        const auto privateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            return qint64(pmc.PrivateUsage);
        };
        const QString name = QStringLiteral("gamehq-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        LocalIntegrationServer server;
        QString error;
        QVERIFY2(server.start(error, name), qPrintable(error));
        QSignalSpy connectedSpy(&server, &LocalIntegrationServer::clientConnected);
        int received = 0;
        connect(&server, &LocalIntegrationServer::messageReceived, this,
                [&](quint64, const integration::Message&) { ++received; });
        QByteArray payload;
        const QByteArray frame = statusFrame(0, 8000);
        constexpr int kFrames = 4000;   // ~32 MB
        payload.reserve(frame.size() * kFrames);
        for (int i = 0; i < kFrames; ++i)
            payload += frame;
        Writer writer(name, payload, 1 << 20);
        QTRY_VERIFY(!connectedSpy.isEmpty());
        const qint64 before = privateBytes();
        QThread::msleep(1500);   // GUI thread stalled while the writer pushes
        const qint64 grew = privateBytes() - before;
        const bool writerDoneDuringStall = writer.written();
        // Writer and server share this process, so the memory figure is only a
        // hint; the proof is that the writer cannot finish while GameHQ stalls.
        qInfo().noquote() << QStringLiteral("%1 MB flood during a 1.5 s stall: writer finished %2, "
                                            "private memory delta %3 KiB")
                                 .arg(payload.size() / (1024 * 1024))
                                 .arg(writerDoneDuringStall ? "yes" : "no").arg(grew / 1024);
        QTRY_COMPARE_WITH_TIMEOUT(received, kFrames, 60000);
        QVERIFY(!writerDoneDuringStall);
        QVERIFY(server.peakBufferedBytes() <= integration::MaximumBufferedBytes);
    }

    void malformedInputMidBurstRejectsAfterTheValidFrames_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("oversized length") << 0;
        QTest::newRow("zero length") << 1;
        QTest::newRow("not JSON") << 2;
        QTest::newRow("unknown type") << 3;
    }

    void malformedInputMidBurstRejectsAfterTheValidFrames()
    {
        QFETCH(int, kind);
        const QString name = QStringLiteral("gamehq-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        LocalIntegrationServer server;
        QString error;
        QVERIFY2(server.start(error, name), qPrintable(error));
        QSignalSpy disconnected(&server, &LocalIntegrationServer::clientDisconnected);
        int received = 0;
        bool afterBad = false;
        connect(&server, &LocalIntegrationServer::messageReceived, this,
                [&](quint64, const integration::Message& m) {
                    ++received;
                    afterBad = afterBad || m.requestId.mid(1).toInt() >= 1000;
                });
        QByteArray payload;
        for (int i = 0; i < 200; ++i)   // more than one read's frame budget
            payload += statusFrame(i, 100);
        QByteArray bad(4, '\0');
        switch (kind) {
        case 0: qToLittleEndian<quint32>(0x7fffffffu, bad.data()); break;
        case 1: break;
        case 2: qToLittleEndian<quint32>(5u, bad.data()); bad += "nope!"; break;
        case 3: {
            QString e;
            bad = integration::encodeFrame({{QStringLiteral("type"), QStringLiteral("evil.thing")}}, e);
            break;
        }
        }
        payload += bad;
        for (int i = 0; i < 50; ++i)
            payload += statusFrame(1000 + i, 100);   // never delivered
        Writer writer(name, payload);
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 5000);
        // Frames before the broken one arrive in budget-sized reads; those
        // decoded together with it go with the connection, as they always did.
        const int atReject = received;
        QVERIFY2(atReject >= 128 && atReject <= 200, qPrintable(QString::number(atReject)));
        QTest::qWait(100);
        QCOMPARE(received, atReject);
        QVERIFY(!afterBad);   // nothing past the broken frame is ever delivered
        QCOMPARE(server.clientCount(), 0);
        QVERIFY(server.peakBufferedBytes() <= integration::MaximumBufferedBytes);
    }

    void aWriterLeavingWithFramesBufferedIsCleanedUp()
    {
        const QString name = QStringLiteral("gamehq-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        LocalIntegrationServer server;
        QString error;
        QVERIFY2(server.start(error, name), qPrintable(error));
        QSignalSpy disconnected(&server, &LocalIntegrationServer::clientDisconnected);
        int received = 0;
        connect(&server, &LocalIntegrationServer::messageReceived, this,
                [&](quint64, const integration::Message&) { ++received; });
        QByteArray payload;
        for (int i = 0; i < 2000; ++i)
            payload += statusFrame(i, 50);
        {
            QSignalSpy connectedSpy(&server, &LocalIntegrationServer::clientConnected);
            Writer writer(name, payload);
            QTRY_VERIFY(!connectedSpy.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(writer.written(), 10000);
            QThread::msleep(200);
        }   // the writer disconnects with frames still waiting to be handled
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 5000);
        const int atDisconnect = received;
        QTest::qWait(200);   // no continuation runs for the gone client
        QCOMPARE(received, atDisconnect);
        QCOMPARE(server.clientCount(), 0);
        // Rapid reconnects keep working afterwards.
        for (int round = 0; round < 20; ++round) {
            QSignalSpy connectedSpy(&server, &LocalIntegrationServer::clientConnected);
            const int before = received;
            Writer w(name, statusFrame(round, 10));
            QTRY_VERIFY(!connectedSpy.isEmpty());
            QTRY_COMPARE(received, before + 1);
        }
    }

    void stoppingWithFramesBufferedDeliversNothingMore()
    {
        const QString name = QStringLiteral("gamehq-ipc-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        auto server = std::make_unique<LocalIntegrationServer>();
        QString error;
        QVERIFY2(server->start(error, name), qPrintable(error));
        int received = 0;
        int atStop = -1;
        LocalIntegrationServer* raw = server.get();
        connect(raw, &LocalIntegrationServer::messageReceived, this,
                [&](quint64, const integration::Message&) {
                    // Shutdown arrives from inside a handler, mid-drain, with
                    // most of the burst still buffered.
                    if (++received == 100) {
                        raw->stop();
                        atStop = received;
                    }
                });
        QByteArray payload;
        for (int i = 0; i < 1000; ++i)
            payload += statusFrame(i, 50);
        QSignalSpy connectedSpy(raw, &LocalIntegrationServer::clientConnected);
        Writer writer(name, payload);
        QTRY_VERIFY(!connectedSpy.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(atStop, 100, 5000);
        QTest::qWait(200);
        QCOMPARE(received, atStop);
        server.reset();
        QTest::qWait(50);
        QCOMPARE(received, atStop);
        QVERIFY(atStop < 1000);
    }
};

QTEST_GUILESS_MAIN(TestIpcBounds)
#include "tst_ipcbounds.moc"
