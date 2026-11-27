#include "gameinput/GameInputEventQueue.h"
#include "gameinput/GameInputQtDispatcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

using namespace ModernInput;

// t35/A06: GameInputEventQueue is bounded, but its dispatcher used to drain it
// on a worker and post one queued Qt callback per batch. While the GUI thread
// was stalled those callbacks piled up in the event loop without limit and
// the queue looked empty, so its overflow recovery never engaged. These tests
// stall the GUI thread on purpose while a producer thread floods the queue.
namespace
{
// Counts the queued deliveries that reach the dispatcher.
class DeliveryCounter : public QObject
{
public:
    int metaCalls = 0;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall)
            ++metaCalls;
        return false;
    }
};

GameInputEvent reading(const QString& device, quint32 buttons, quint64 timestamp)
{
    GameInputEvent e;
    e.kind = GameInputEventKind::Reading;
    e.deviceId = device;
    e.standardButtons = buttons;
    e.timestamp = timestamp;
    return e;
}

GameInputEvent edge(const QString& device, bool pressed, int index)
{
    GameInputEvent e;
    e.kind = pressed ? GameInputEventKind::SystemButtonPressed
                     : GameInputEventKind::SystemButtonReleased;
    e.deviceId = device;
    e.controlId = QStringLiteral("gamepad.capture");
    e.timestamp = quint64(index);
    return e;
}

GameInputEvent lifecycle(GameInputEventKind kind, const QString& device)
{
    GameInputEvent e;
    e.kind = kind;
    e.deviceId = device;
    return e;
}

struct Fixture
{
    std::shared_ptr<GameInputEventQueue> queue;
    std::unique_ptr<GameInputQtDispatcher> dispatcher;
    DeliveryCounter counter;
    QVector<GameInputEventBatch> batches;

    explicit Fixture(int capacity = 256, int reserve = 64)
        : queue(std::make_shared<GameInputEventQueue>(capacity, reserve))
        , dispatcher(std::make_unique<GameInputQtDispatcher>(queue))
    {
        dispatcher->installEventFilter(&counter);
        QObject::connect(dispatcher.get(), &GameInputQtDispatcher::eventsReady, dispatcher.get(),
                         [this](const GameInputEventBatch& b) { batches.append(b); });
        dispatcher->start();
    }

    // Blocks the GUI thread for stallMs while produce() runs on another
    // thread, like a long layout pass while a controller streams readings.
    template <typename Produce>
    void stallWhile(int stallMs, Produce produce)
    {
        std::thread producer(produce);
        QThread::msleep(static_cast<unsigned long>(stallMs));
        producer.join();
    }

    int delivered() const
    {
        int n = 0;
        for (const auto& b : batches)
            n += b.events.size();
        return n;
    }

    void settle()
    {
        // Let every pending delivery run; a lost wakeup would leave events here.
        QTRY_VERIFY_WITH_TIMEOUT(queue->pendingCount() == 0, 3000);
        QTest::qWait(30);
    }
};

void paced(int count, int durationMs, const std::function<void(int)>& push)
{
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < count; ++i) {
        push(i);
        const qint64 due = qint64(i + 1) * durationMs / count;
        while (timer.elapsed() < due)
            std::this_thread::yield();
    }
}
} // namespace

class TestGameInputDelivery : public QObject
{
    Q_OBJECT

private slots:
    void aStalledGuiHoldsOneDeliveryAndBoundedReadings_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<int>("stallMs");
        for (int rate : {1000, 4000, 8000}) {
            for (int stall : {200, 1000})
                QTest::addRow("%d/s, %d ms stall", rate, stall) << rate << stall;
        }
    }

    void aStalledGuiHoldsOneDeliveryAndBoundedReadings()
    {
        // Two pads stream button-stable readings: each pad's first reading is
        // an edge (no previous state), the rest coalesce to the newest one.
        QFETCH(int, rate);
        QFETCH(int, stallMs);
        Fixture f;
        const int count = rate * stallMs / 1000;
        int maxPending = 0;
        f.stallWhile(stallMs, [&] {
            paced(count, stallMs, [&](int i) {
                f.queue->push(reading(i % 2 ? QStringLiteral("pad-b") : QStringLiteral("pad-a"),
                                      0x4, quint64(i)));
                maxPending = std::max(maxPending, f.queue->pendingCount());
            });
        });
        f.settle();
        qInfo() << count << "readings during a" << stallMs << "ms stall:" << f.counter.metaCalls
                << "queued deliveries," << f.batches.size() << "batches," << f.delivered()
                << "events delivered, source backlog peak" << maxPending;
        QVERIFY2(f.counter.metaCalls <= 2, qPrintable(QString::number(f.counter.metaCalls)));
        QVERIFY(maxPending <= 4);
        QVERIFY(f.delivered() <= 4);   // 2 first-reading edges + newest per pad
        for (const auto& b : f.batches)
            QVERIFY(!b.forceResync);
        // The newest reading of each pad arrived.
        quint64 lastA = 0, lastB = 0;
        for (const auto& b : f.batches) {
            for (const auto& e : b.events)
                (e.deviceId == QStringLiteral("pad-a") ? lastA : lastB) = e.timestamp;
        }
        QCOMPARE(lastA, quint64(count - 2));
        QCOMPARE(lastB, quint64(count - 1));
    }

    void edgesDuringAStallArriveInOrder()
    {
        Fixture f;
        constexpr int kEdges = 200;   // within the 256 + 64 bound
        f.stallWhile(200, [&] {
            paced(kEdges, 150, [&](int i) { f.queue->push(edge(QStringLiteral("pad"), i % 2 == 0, i)); });
        });
        f.settle();
        QVERIFY2(f.counter.metaCalls <= 2, qPrintable(QString::number(f.counter.metaCalls)));
        QCOMPARE(f.delivered(), kEdges);
        int expected = 0;
        for (const auto& b : f.batches) {
            QVERIFY(!b.forceResync);
            for (const auto& e : b.events) {
                QCOMPARE(int(e.timestamp), expected);
                QCOMPARE(e.kind, expected % 2 == 0 ? GameInputEventKind::SystemButtonPressed
                                                   : GameInputEventKind::SystemButtonReleased);
                ++expected;
            }
        }
    }

    void anEdgeFloodBeyondTheBoundRecoversExplicitly()
    {
        // More discrete edges than the queue holds during a 1 s stall: the
        // consumer must get an explicit resync, not a silent partial stream,
        // and delivery works normally afterwards.
        Fixture f;
        f.stallWhile(1000, [&] {
            paced(1000, 800, [&](int i) { f.queue->push(edge(QStringLiteral("pad"), i % 2 == 0, i)); });
        });
        f.settle();
        QVERIFY2(f.counter.metaCalls <= 2, qPrintable(QString::number(f.counter.metaCalls)));
        QVERIFY(f.delivered() <= 256 + 64 + 1);
        bool resync = false;
        bool recovery = false;
        for (const auto& b : f.batches) {
            resync = resync || b.forceResync;
            for (const auto& e : b.events)
                recovery = recovery || (e.kind == GameInputEventKind::RecoveryRequired
                                        && e.deviceId == QStringLiteral("pad"));
        }
        QVERIFY(resync);
        QVERIFY(recovery);

        f.batches.clear();
        f.queue->push(edge(QStringLiteral("pad"), true, 5000));
        QTRY_COMPARE(f.batches.size(), 1);
        QVERIFY(!f.batches.first().forceResync);
        QCOMPARE(f.batches.first().events.size(), 1);
    }

    void hotplugDuringAStallKeepsLifecycleOrder()
    {
        Fixture f;
        f.stallWhile(200, [&] {
            f.queue->push(lifecycle(GameInputEventKind::DeviceAdded, QStringLiteral("pad")));
            for (int i = 0; i < 500; ++i)
                f.queue->push(reading(QStringLiteral("pad"), 0, quint64(i)));
            f.queue->push(lifecycle(GameInputEventKind::DeviceRemoved, QStringLiteral("pad")));
            f.queue->push(lifecycle(GameInputEventKind::DeviceAdded, QStringLiteral("pad")));
            f.queue->push(reading(QStringLiteral("pad"), 0x1, 900));
        });
        f.settle();
        QList<GameInputEventKind> kinds;
        for (const auto& b : f.batches) {
            QVERIFY(!b.forceResync);
            for (const auto& e : b.events)
                kinds.append(e.kind);
        }
        // The pre-removal readings never trail the removal.
        const qsizetype removed = kinds.indexOf(GameInputEventKind::DeviceRemoved);
        QVERIFY(removed > 0);
        QCOMPARE(kinds.first(), GameInputEventKind::DeviceAdded);
        QCOMPARE(kinds.at(removed + 1), GameInputEventKind::DeviceAdded);
        QCOMPARE(kinds.last(), GameInputEventKind::Reading);
        QVERIFY(f.counter.metaCalls <= 2);
    }

    void noWakeupIsLostWhileTheGuiDrainsAndStalls()
    {
        // Producers race the drain: events land while a drain is pending,
        // while it takes, and right after. Every edge must arrive exactly
        // once, in order, with never more than one delivery outstanding.
        Fixture f(8192, 128);
        constexpr int kPads = 2;
        constexpr int kEdges = 4000;
        std::atomic_int maxOutstanding{0};
        std::atomic_bool done{false};
        QTimer stall;
        stall.setInterval(1);
        int tick = 0;
        QObject::connect(&stall, &QTimer::timeout, &stall, [&] {
            QThread::usleep(static_cast<unsigned long>((++tick % 7) * 300));
        });
        stall.start();
        std::thread producer([&] {
            paced(kEdges, 1500, [&](int i) {
                f.queue->push(edge(QStringLiteral("pad-%1").arg(i % kPads), (i / kPads) % 2 == 0, i));
                maxOutstanding = std::max(maxOutstanding.load(), f.dispatcher->pendingDeliveries());
            });
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done.load(), 10000);
        producer.join();
        f.settle();
        stall.stop();
        QVERIFY(maxOutstanding.load() <= 1);
        QVERIFY2(f.batches.size() > 50, "the drains must interleave with production");
        QCOMPARE(f.dispatcher->pendingDeliveries(), 0);
        QCOMPARE(f.delivered(), kEdges);
        int expected = 0;
        for (const auto& b : f.batches) {
            QVERIFY(!b.forceResync);
            for (const auto& e : b.events)
                QCOMPARE(int(e.timestamp), expected++);
        }
        qInfo() << kEdges << "edges in" << f.batches.size() << "drains, max outstanding"
                << maxOutstanding.load();
    }

    void shutdownWithADeliveryPendingIsPromptAndSilent()
    {
        auto f = std::make_unique<Fixture>();
        f->stallWhile(200, [&] {
            for (int i = 0; i < 100; ++i)
                f->queue->push(edge(QStringLiteral("pad"), i % 2 == 0, i));
        });
        QElapsedTimer timer;
        timer.start();
        f->dispatcher.reset();   // a delivery is still queued for it
        QVERIFY(timer.elapsed() < 500);
        QCoreApplication::processEvents();
        QTest::qWait(20);
        QVERIFY(f->batches.isEmpty());
        QVERIFY(!f->queue->accepting());
    }
};

QTEST_GUILESS_MAIN(TestGameInputDelivery)
#include "tst_gameinputdelivery.moc"
