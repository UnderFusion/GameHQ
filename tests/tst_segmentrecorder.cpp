#include "capture/SegmentRecorder.h"
#include "capture/ReplayExportTask.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <stdexcept>
#include <thread>

class TestSegmentRecorder : public QObject
{
    Q_OBJECT

    static QString segment(const QString& dir, int index, bool stale = false)
    {
        const QString path = dir + QStringLiteral("/%1_clip.mp4").arg(index, 4, 10, QLatin1Char('0'));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray::number(index)) <= 0)
            qFatal("Cannot create segment fixture");
        if (stale && !file.setFileTime(QDateTime::currentDateTime().addSecs(-1200),
                                      QFileDevice::FileModificationTime))
            qFatal("Cannot age segment fixture");
        return path;
    }

    static void restore(SegmentRecorder& recorder, const QString& dir, int keep)
    {
        recorder.m_cacheDir = dir;
        recorder.m_keepSegments = keep;
        recorder.restoreRing(); // the same disk restore/prune used by begin()
    }

private slots:
    void replacementRecorderRetainsLeasedStalePaths()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 8; ++i)
            paths << segment(dir.path(), i, i < 3);
        SegmentLease lease(paths.first(2));
        auto oldRecorder = std::make_unique<SegmentRecorder>();
        oldRecorder->m_segments = paths.first(2);
        oldRecorder.reset();

        SegmentRecorder replacement;
        restore(replacement, dir.path(), 2);
        QCOMPARE(replacement.m_segments, (QStringList{paths[0], paths[1], paths[6], paths[7]}));
        for (int i = 0; i < paths.size(); ++i)
            QCOMPARE(QFile::exists(paths[i]), i < 2 || i >= 6);
        // Re-arm the same recorder and shrink the window while the export owns it.
        restore(replacement, dir.path(), 1);
        QCOMPARE(replacement.m_segments, (QStringList{paths[0], paths[1], paths[7]}));
        lease = {};
        replacement.trimRing();
        QCOMPARE(replacement.m_segments, QStringList{paths[7]});
    }

    void overlappingLeasesReleaseOnlyTheirOwnPaths()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 8; ++i)
            paths << segment(dir.path(), i);
        SegmentLease first(paths.first(2));
        SegmentLease second({paths[1], paths[2]});
        QVERIFY(first.ownerId() != second.ownerId());
        SegmentRecorder recorder;
        restore(recorder, dir.path(), 2);
        QCOMPARE(recorder.m_segments.size(), 5);
        first = {};
        recorder.trimRing();
        QCOMPARE(recorder.m_segments, (QStringList{paths[1], paths[2], paths[6], paths[7]}));
        second = {};
        recorder.trimRing();
        QCOMPARE(recorder.m_segments, paths.last(2));
    }

    void oldLeaseDoesNotBlockUnrelatedTrimming()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString old = segment(dir.path(), 0);
        SegmentLease lease({old});
        SegmentRecorder recorder;
        restore(recorder, dir.path(), 2);
        for (int i = 1; i <= 20; ++i) {
            recorder.m_segments << segment(dir.path(), i);
            recorder.trimRing();
            QCOMPARE(recorder.m_segments.size(), qMin(i + 1, 3));
            QVERIFY(QFile::exists(old));
            QCOMPARE(QDir(dir.path()).entryList({"*_clip.mp4"}, QDir::Files).size(),
                     recorder.m_segments.size());
        }
    }

    void normalizedPathsShareProtection()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        QString alias = dir.path() + "/./0000_clip.mp4";
#ifdef Q_OS_WIN
        alias = QDir::toNativeSeparators(alias.toUpper());
#endif
        SegmentLease lease({alias, alias});
        QVERIFY(!SegmentLease::removeIfUnleased(path));
        lease = {};
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }

    void exportReadsCompleteSnapshotAfterRecorderReplacement()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 3; ++i)
            paths << segment(dir.path(), i);
        auto recorder = std::make_unique<SegmentRecorder>();
        restore(*recorder, dir.path(), 3);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto bytes = std::make_shared<QByteArray>();
        ReplayExportTask task(SegmentLease(recorder->m_segments),
            [entered, resume, bytes](const QStringList& snapshot) {
                entered->release();
                resume->acquire();
                for (const QString& path : snapshot) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly))
                        *bytes += file.readAll();
                }
            });
        // Always unblock before the task destructor, including assertion exits.
        const auto unblock = qScopeGuard([resume] { resume->release(); });
        task.start();
        QVERIFY(entered->tryAcquire(1, 2000));
        recorder.reset();
        for (int i = 3; i < 8; ++i)
            segment(dir.path(), i);
        recorder = std::make_unique<SegmentRecorder>();
        restore(*recorder, dir.path(), 2);
        QCOMPARE(recorder->m_segments.size(), 5);
        resume->release();
        QVERIFY(task.wait(2000));
        QCOMPARE(*bytes, QByteArray("012"));
        // Task still exists, and no finished callback has run: lease is gone.
        recorder->trimRing();
        QCOMPARE(recorder->m_segments.size(), 2);
        for (const QString& path : paths)
            QVERIFY(!QFile::exists(path));
    }

    void leaseCleanupWithoutCompletionCallback_data()
    {
        QTest::addColumn<bool>("throws");
        QTest::newRow("early-return") << false;
        QTest::newRow("exception") << true;
    }

    void leaseCleanupWithoutCompletionCallback()
    {
        QFETCH(bool, throws);
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        ReplayExportTask task(SegmentLease({path}), [throws](const QStringList&) {
            if (throws)
                throw std::runtime_error("simulated export failure");
            return;
        });
        bool callbackRan = false;
        auto receiver = std::make_unique<QObject>();
        connect(&task, &QThread::finished, receiver.get(), [&] { callbackRan = true; },
                Qt::QueuedConnection);
        receiver.reset();
        if (throws)
            QTest::ignoreMessage(QtWarningMsg, "ReplayExportTask: export failed with an exception");
        task.start();
        QVERIFY(task.wait(2000));
        QCoreApplication::processEvents();
        QVERIFY(!callbackRan);
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }

    void shutdownJoinsBeyondGracePeriod()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto completed = std::make_shared<std::atomic_bool>(false);
        ReplayExportTask task(SegmentLease({path}),
            [entered, resume, completed](const QStringList&) {
                entered->release();
                resume->acquire();
                *completed = true;
            });
        const auto unblock = qScopeGuard([resume] { resume->release(); });
        task.start();
        QVERIFY(entered->tryAcquire(1, 2000));
        std::jthread release([resume] {
            QThread::msleep(100);
            resume->release();
        });
        QTest::ignoreMessage(QtWarningMsg,
                            "ReplayExportTask: shutdown grace period elapsed; waiting for export");
        QVERIFY(!task.finish(1));
        QVERIFY(completed->load());
        QVERIFY(task.isFinished());
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }
};

QTEST_GUILESS_MAIN(TestSegmentRecorder)
#include "tst_segmentrecorder.moc"
