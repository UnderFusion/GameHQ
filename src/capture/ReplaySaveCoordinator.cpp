// FramePumpWorker replay save path: preflight, ring freeze, export hand-off and
// the update-preparation gate that waits for an export in flight.
#include "capture/FramePumpPipeline.h"

#include "capture/CapturePublisher.h"
#include "capture/ReplayBufferState.h"
#include "capture/ReplayExporter.h"
#include "capture/ReplayExportTask.h"
#include "capture/SegmentLease.h"
#include "capture/SegmentRecorder.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"
#include "storage/ThumbnailService.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QThread>

#include <memory>
#include <utility>


// Preflight: refuse when the buffer is not running or an export is in flight.
bool FramePumpWorker::saveGuard(const QString& saveId)
{
    checkReplayReadiness();
    if (!ReplayBufferState::canSave(m_bufferState)) {
        emit clipFailed(QStringLiteral("Replay"), !m_audioFailure.isEmpty()
                            ? m_audioFailure
                            : ReplayBufferState::saveRejection(m_bufferState));
        return false;
    }
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: request begin pipe=%2 recorder=%3 active=%4")
                             .arg(saveId)
                             .arg(m_pipe ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(m_pipe && m_pipe->recorder ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(m_pipe && m_pipe->recorder && m_pipe->recorder->isActive()
                                  ? QStringLiteral("yes") : QStringLiteral("no"));

    if (!m_pipe || !m_pipe->recorder || !m_pipe->recorder->isActive()) {
        qInfo() << "FramePump: save-replay ignored — buffer not running (always-on recording is controlled in Settings → Replay)";
        emit clipFailed(QStringLiteral("Replay"), QStringLiteral("Replay buffer is not running"));
        return false;
    }
    if (m_exportBusy) {
        qInfo() << "FramePump: save-replay ignored — an export is already in progress";
        emit clipFailed(m_pipe->gameName.isEmpty() ? QStringLiteral("Replay") : m_pipe->gameName,
                        QStringLiteral("A replay save is already in progress"));
        return false;
    }
    if (m_updatePreparing) {
        emit clipFailed(m_pipe->gameName.isEmpty() ? QStringLiteral("Replay") : m_pipe->gameName,
                        QStringLiteral("Replay capture is paused for an update"));
        return false;
    }
    return true;
}

// Freeze the ring (finalizes the in-flight segment, keeps recording).
// The snapshot itself leases its paths before recording resumes. Empty result
// means the ring had nothing; the failure signal is already emitted.
SegmentLease FramePumpWorker::freezeRing(const QString& saveId)
{
    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    SegmentLease lease = m_pipe->recorder->snapshotForSave();
    checkReplayReadiness();
    const QStringList& segs = lease.paths();
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: frozen segments=%2 elapsedMs=%3")
                             .arg(saveId).arg(segs.size()).arg(snapshotTimer.elapsed());
    for (int i = 0; i < segs.size(); ++i) {
        const QFileInfo fi(segs.at(i));
        qInfo().noquote() << QStringLiteral("ReplaySave[%1]: snapshot segment %2 path=%3 exists=%4 bytes=%5")
                                 .arg(saveId).arg(i).arg(segs.at(i))
                                 .arg(fi.exists() ? QStringLiteral("yes") : QStringLiteral("no"))
                                 .arg(fi.exists() ? fi.size() : -1);
    }
    if (segs.isEmpty()) {
        qWarning() << "FramePump: save-replay — ring empty, nothing to save";
        const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Replay")
                                                        : m_pipe->gameName;
        emit clipFailed(game, QStringLiteral("Replay buffer is empty"));
        return {};
    }
    if (!m_pipe->recorder->hasClosedMedia()) {
        emit clipFailed(m_pipe->gameName, ReplayBufferState::saveRejection(m_bufferState));
        return {};
    }
    return lease;
}

// Export-thread fallback preview from the newest leased segment. Static and
// value-only: decoding must never access the capture worker or its pipeline.
QString FramePumpWorker::instantThumbnail(const QString& lastSegment, const QString& thumbPath,
                                          const QString& saveId)
{
    QString instantThumb;
    QImage instantFrame;
    QElapsedTimer instantThumbTimer;
    instantThumbTimer.start();
    if (ReplayExporter::grabThumbnail(lastSegment, instantFrame) && !instantFrame.isNull()) {
        const QImage scaled = instantFrame.width() > 640
            ? instantFrame.scaledToWidth(640, Qt::SmoothTransformation) : instantFrame;
        if (ThumbnailService::saveThumbnail(scaled, thumbPath, "PNG"))
            instantThumb = thumbPath;
    }
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: instant thumbnail ok=%2 path=%3 elapsedMs=%4")
                             .arg(saveId)
                             .arg(instantThumb.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                             .arg(instantThumb)
                             .arg(instantThumbTimer.elapsed());
    return instantThumb;
}

// Remux + final thumbnail on their OWN thread: previously they ran right
// here on the capture thread, so every save paused recording (and any
// pad/frame processing) for the whole export. The task owns the snapshot lease
// and releases it even if the worker completion callback is never delivered.
void FramePumpWorker::runExport(SegmentLease lease,
                                const CapturePublisher::Reservation& reservation,
                                const QString& thumbPath,
                                const QString& game, const QString& exePath,
                                const QString& saveId, quint64 requestId, bool requireAudio)
{
    const QString outPath = reservation.finalPath;
    const QString partialPath = reservation.pendingPath;
    struct ExportResult {
        bool ok = false;
        QString finalThumb;
    };
    auto result = std::make_shared<ExportResult>();
    m_exportBusy = true;
    emit exportBusyChanged(true);
    const quint64 generation = ++m_exportGeneration;

    m_exportTask = std::make_unique<ReplayExportTask>(std::move(lease),
        [reservation, outPath, partialPath, thumbPath, saveId, result, requireAudio]
        (const QStringList& segs) {
            const auto discardOnFailure = qScopeGuard([&] {
                if (!result->ok)
                    CapturePublisher::discard(reservation);
            });
            const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const auto uninitialize = qScopeGuard([hrCo] {
                if (SUCCEEDED(hrCo))
                    CoUninitialize();
            });
            QElapsedTimer t;
            t.start();
            // Both preview and final decode/scale/encode stay under the export
            // task's lease. The capture worker only schedules this work.
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            if (!segs.isEmpty())
                result->finalThumb = instantThumbnail(segs.last(), thumbPath, saveId);
            // The reserved .part already exists and the MP4 sink writer
            // truncates it, so the name stays claimed for the whole export.
            if (ReplayExporter::concat(segs, partialPath, saveId, // remux, no re-encode
                                       requireAudio ? ReplayExporter::Audio::Required
                                                    : ReplayExporter::Audio::Any)) {
                qInfo().noquote() << QStringLiteral(
                    "ReplaySave[%1]: remux ok output=%2 segments=%3 elapsedMs=%4 bytes=%5")
                    .arg(saveId).arg(partialPath).arg(segs.size()).arg(t.elapsed())
                    .arg(QFileInfo(partialPath).size());
                // Final thumbnail: the clip's actual first frame, replacing
                // the instant preview (same file path).
                QImage frame;
                if (ReplayExporter::grabThumbnail(partialPath, frame) && !frame.isNull()) {
                    const QImage scaled = frame.width() > 640
                        ? frame.scaledToWidth(640, Qt::SmoothTransformation) : frame;
                    if (ThumbnailService::saveThumbnail(scaled, thumbPath, "PNG"))
                        result->finalThumb = thumbPath;
                }
                // Publish only a completely finalized file. A crash before
                // this point leaves an ignored *.part, never a gallery MP4.
                // The rename refuses to clobber, so an unrelated clip that
                // somehow holds the name survives instead of being replaced.
                QString publishError;
                result->ok = CapturePublisher::publish(reservation, &publishError);
                if (!result->ok)
                    qWarning() << "ReplaySave: could not publish finalized clip" << outPath
                               << publishError;
            } else {
                qWarning().noquote() << QStringLiteral(
                    "ReplaySave[%1]: failed - remux failed output=%2 elapsedMs=%3")
                    .arg(saveId).arg(outPath).arg(t.elapsed());
            }
        });

    // Completion runs back on THIS worker thread (context object = this), so
    // recorder access needs no locking; the connection dissolves safely if
    // the worker is destroyed first.
    connect(m_exportTask.get(), &QThread::finished, this,
            [this, result, outPath, game, exePath, saveId, generation, requestId] {
                if (generation != m_exportGeneration)
                    return;
                m_exportTask.reset(); // finished; join and destroy on its owner thread
                m_exportBusy = false;
                emit exportBusyChanged(false);
                if (m_pipe && m_pipe->recorder)
                    m_pipe->recorder->trimRing();
                if (result->ok) {
                    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: published").arg(saveId);
                    emit clipSaved(outPath, game, result->finalThumb, exePath);
                } else {
                    qWarning() << "FramePump: save-replay — remux failed for" << outPath;
                    emit clipFailed(game, QStringLiteral("Could not export a complete replay clip"));
                }
                emit saveRequestFinished(requestId);
                if (m_updatePreparing) {
                    stopPump();
                    emit updateReady(m_updateGeneration);
                }
            }, Qt::QueuedConnection);
    m_exportTask->start();
}

void FramePumpWorker::prepareForUpdate(quint64 generation)
{
    m_updateGeneration = generation;
    m_updatePreparing = true;
    if (m_exportBusy)
        return;
    stopPump();
    emit updateReady(m_updateGeneration);
}

void FramePumpWorker::cancelUpdatePreparation()
{
    m_updatePreparing = false;
}

void FramePumpWorker::saveReplayOnWorker(const QString& clipsBaseRoot, quint64 generation,
                                        quint64 requestId, quint64 chainId)
{
    QElapsedTimer saveTimer;
    saveTimer.start();
    // Measure the next turn of this worker's event loop, not just thread start.
    // This is save-request instrumentation; poll()/frame arrival are untouched.
    const auto reportReturn = qScopeGuard([this, saveTimer, chainId, requestId] {
        QMetaObject::invokeMethod(this, [saveTimer, chainId, requestId] {
            qInfo().noquote() << QStringLiteral("ReplaySave[%1]: worker resumed elapsedMs=%2")
                .arg(chainId ? chainId : requestId).arg(saveTimer.elapsed());
        }, Qt::QueuedConnection);
    });
    bool handedToExport = false;
    const auto finishRejectedRequest = qScopeGuard([&] {
        if (!handedToExport)
            emit saveRequestFinished(requestId);
    });
    if (generation != m_pumpGeneration) {
        emit clipFailed(QStringLiteral("Replay"),
                        QStringLiteral("Replay buffer changed before the save could start"));
        return;
    }
    // The chain id from the press, so every line below joins the same chain the
    // service already logged. Falls back to the lease token for callers that
    // have no request behind them.
    const QString saveId = QString::number(chainId ? chainId : requestId);
    if (!saveGuard(saveId))
        return;

    // Recording may contain a short first segment, even before Ready. Freeze
    // finalizes that footage and rejects an empty/unusable snapshot itself.
    SegmentLease lease = freezeRing(saveId);
    if (lease.isEmpty())
        return;

    const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Unknown Game")
                                                    : m_pipe->gameName;

    const QString clipsDir = clipsBaseRoot + QLatin1Char('/')
                             + GameIdentity::pathComponent(game) + QStringLiteral("/Clips");
    if (!GameIdentity::isInsideRoot(clipsBaseRoot, clipsDir)) {
        emit clipFailed(game, QStringLiteral("The game's clips folder would be outside the clips root"));
        return;
    }
    if (!QDir().mkpath(clipsDir)) {
        emit clipFailed(game, QStringLiteral("Could not create the selected clips folder"));
        return;
    }
    // The name is claimed before anything is written, so a second save inside
    // the same second is handed <stamp>_2.mp4 instead of the first clip's name.
    // Nothing below may delete or overwrite a file it did not reserve.
    const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
        clipsDir, QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
    if (!reservation.isValid()) {
        emit clipFailed(game, QStringLiteral("Could not reserve a name for the clip"));
        return;
    }
    // Derived from the reserved clip name, so the preview inherits the same
    // _2 and stays the "<clip base>_clip.png" pair ThumbnailService reattaches.
    const QString thumbPath = CapturePublisher::companionPath(
        reservation.finalPath, Paths::thumbnailsDir(), QStringLiteral("_clip.png"));
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: output path=%2 game=%3")
                             .arg(saveId).arg(reservation.finalPath).arg(game);

    // Receipt already happened at service entry. No image work is needed to
    // announce that export started; clipSaved delivers the completed thumbnail.
    emit clipSaving(game, QString(), m_pipe->executablePath);

    // A recorder with an audio track must export one; the exporter refuses
    // any frozen segment that lacks it instead of saving a silent clip.
    runExport(std::move(lease), reservation, thumbPath,
              game, m_pipe->executablePath, saveId, requestId,
              m_pipe->audio && m_pipe->recorder->hasAudio());
    handedToExport = true;
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: exporting totalSoFarMs=%2")
                             .arg(saveId).arg(saveTimer.elapsed());
}

