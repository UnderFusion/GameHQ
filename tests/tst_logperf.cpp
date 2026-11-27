#include "diagnostics/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <memory>
#include <vector>

// t43/A13: what a log line costs its caller. Logger is process-global, so this
// suite installs once into its own temporary directory.
namespace
{
struct Latency
{
    std::vector<qint64> ns;
    void add(qint64 v) { ns.push_back(v); }
    qint64 pct(double p)
    {
        if (ns.empty())
            return 0;
        std::sort(ns.begin(), ns.end());
        return ns[std::min(ns.size() - 1, size_t(p * double(ns.size())))];
    }
};

// Logger owns the message handler here, so results go straight to stdout.
void report(const QString& line)
{
    std::printf("%s\n", qPrintable(line));
    std::fflush(stdout);
}

QString us(qint64 ns) { return QString::number(double(ns) / 1000.0, 'f', 1); }
} // namespace

class TestLogPerf : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString logs() const { return m_dir.filePath(QStringLiteral("logs")); }
    qint64 totalLogBytes() const
    {
        qint64 total = 0;
        for (const QFileInfo& f : QDir(logs()).entryInfoList(QDir::Files))
            total += f.size();
        return total;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        Logger::install(logs());
        QVERIFY(Logger::writingToFile());
    }

    void singleThreadCallerLatency()
    {
        const QString payload(80, QLatin1Char('a'));
        Latency lat;
        QElapsedTimer t;
        for (int i = 0; i < 20000; ++i) {
            t.start();
            qInfo().noquote() << "Bench: line" << i << payload;
            lat.add(t.nsecsElapsed());
        }
        report(QStringLiteral("RESULT single thread: p50 %1 us, p99 %2 us, max %3 us")
                                    .arg(us(lat.pct(0.5)), us(lat.pct(0.99)), us(lat.pct(1.0))));
    }

    void concurrentProducersCallerLatency()
    {
        constexpr int kThreads = 4;
        constexpr int kLines = 5000;
        std::vector<Latency> per(kThreads);
        std::vector<std::unique_ptr<QThread>> threads;
        std::atomic_int ready{0};
        QElapsedTimer wall;
        wall.start();
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back(QThread::create([&, t] {
                ++ready;
                while (ready.load() < kThreads) {}
                const QString payload(80, QLatin1Char('b'));
                QElapsedTimer c;
                for (int i = 0; i < kLines; ++i) {
                    c.start();
                    qInfo().noquote() << "Bench: thread" << t << i << payload;
                    per[size_t(t)].add(c.nsecsElapsed());
                }
            }));
            threads.back()->start();
        }
        for (auto& th : threads)
            QVERIFY(th->wait(60000));
        Latency all;
        for (auto& p : per)
            for (qint64 v : p.ns)
                all.add(v);
        report(QStringLiteral("RESULT %1 threads x %2: p50 %3 us, p99 %4 us, max %5 us, wall %6 ms")
                                    .arg(kThreads).arg(kLines)
                                    .arg(us(all.pct(0.5)), us(all.pct(0.99)), us(all.pct(1.0)))
                                    .arg(wall.elapsed()));
        // Every line from every thread is whole: one write per line, CRLF.
        QFile f(Logger::filePath(logs()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = f.readAll().split('\n');
        int benchLines = 0;
        for (const QByteArray& line : lines) {
            if (!line.contains("Bench: thread"))
                continue;
            ++benchLines;
            QVERIFY2(line.endsWith('\r'), line.constData());
            QCOMPARE(line.count("Bench:"), 1);
            QVERIFY(line.contains(QByteArray(80, 'b')));
        }
        QCOMPARE(benchLines, kThreads * kLines);
    }

    void rotationCountsEncodedBytes()
    {
        // Polish text is two bytes per character in UTF-8. The file must
        // rotate at kMaxLogBytes of real bytes, not of characters.
        const QString payload = QString(100, QChar(0x017C));   // 'ż'
        const QString current = Logger::filePath(logs());
        qint64 peak = 0;
        for (int i = 0; i < 60000; ++i) {
            qInfo().noquote() << payload;
            if (i % 500 == 0)
                peak = std::max(peak, QFileInfo(current).size());
        }
        peak = std::max(peak, QFileInfo(current).size());
        report(QStringLiteral("RESULT unicode: largest gamehq.log seen %1 bytes (limit %2)")
                                    .arg(peak).arg(Logger::kMaxLogBytes));
        QVERIFY2(peak <= Logger::kMaxLogBytes + 64 * 1024, qPrintable(QString::number(peak)));
        // The read-back text is intact.
        QFile f(current);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY(text.contains(payload));
    }
};

QTEST_GUILESS_MAIN(TestLogPerf)
#include "tst_logperf.moc"
