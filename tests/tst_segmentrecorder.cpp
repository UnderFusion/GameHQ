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
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

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
    void earlySnapshot_data()
    {
        QTest::addColumn<bool>("hasFrames");
        QTest::newRow("empty-is-not-saveable") << false;
        QTest::newRow("one-second-of-thirty-is-saveable") << true;
    }

    void earlySnapshot()
    {
        QFETCH(bool, hasFrames);
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-partial-XXXXXX");
        QVERIFY(dir.isValid());
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const QByteArray pixels(320 * 180 * 4, char(0x80));
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.constData();
        data.SysMemPitch = 320 * 4;
        ComPtr<ID3D11Texture2D> texture;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 5, 30,
                               dir.path(), device.Get(), context.Get()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        if (hasFrames) {
            for (int i = 0; i < 30; ++i)
                recorder.writeFrame(texture.Get(), device.Get(), context.Get(),
                                    10000000LL + i * 10000000LL / 30);
        }
        QVERIFY(!recorder.hasClosedMedia());
        const SegmentLease clip = recorder.snapshotForSave();
        QCOMPARE(recorder.hasClosedMedia(), hasFrames);
        QCOMPARE(clip.isEmpty(), !hasFrames);
        QVERIFY(recorder.isActive()); // saving must leave recording usable
        if (!hasFrames)
            return;
        QCOMPARE(clip.paths().size(), 1);
        const QString path = clip.paths().first();
        ComPtr<IMFSourceReader> reader;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(
            reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, reader.GetAddressOf())));
        PROPVARIANT duration{};
        const auto clearDuration = qScopeGuard([&] { PropVariantClear(&duration); });
        QVERIFY(SUCCEEDED(reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)));
        QCOMPARE(duration.vt, VARTYPE(VT_UI8));
        QVERIFY(duration.uhVal.QuadPart > 0);
        QVERIFY(duration.uhVal.QuadPart < 50000000ULL); // shorter than first segment
        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        QVERIFY(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, nullptr, &flags, &timestamp, sample.GetAddressOf())));
        QVERIFY(sample.Get() != nullptr); // finalized file really contains video
    }

    void readinessRequiresConfirmedMediaInTheNormalWindow()
    {
        SegmentRecorder recorder;
        recorder.m_keepSegments = 2;
        recorder.m_segments = {"old-leased", "recent", "newest"};
        QVERIFY(!recorder.hasClosedMedia()); // restored filenames alone prove nothing
        recorder.m_lastClosedMediaPath = "old-leased";
        QVERIFY(!recorder.hasClosedMedia()); // retained lease is outside a new save
        recorder.m_lastClosedMediaPath = "recent";
        QVERIFY(recorder.hasClosedMedia());
        recorder.m_segments << "next";
        QVERIFY(!recorder.hasClosedMedia()); // old usable video rolled out of the window
        recorder.m_lastClosedMediaPath = "next";
        QVERIFY(recorder.hasClosedMedia());
    }

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
