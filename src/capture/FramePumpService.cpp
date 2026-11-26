#include "capture/FramePumpService.h"

#include "capture/AudioCapture.h"
#include "capture/AudioSourcePolicy.h"
#include "capture/CaptureBorderState.h"
#include "capture/CapturePublisher.h"
#include "config/ConfigKeys.h"
#include "config/CaptureLocations.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "games/GameDetector.h"

#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <utility>

// The FramePumpWorker implementation is split across FramePumpPipeline.cpp
// (lifecycle, bring-up, poll) and ReplaySaveCoordinator.cpp (save path).

// How long an unfinished clip file may sit in a Clips folder before it counts
// as abandoned. Same 10 minutes the segment cache uses.
constexpr qint64 kStaleClipPartMaxAgeSecs = 600;

namespace
{
QSize parseResolution(const QString& value, const QSize& fallback)
{
    const QStringList parts = value.toLower().split(QLatin1Char('x'));
    if (parts.size() != 2)
        return fallback;
    bool okW = false;
    bool okH = false;
    const int w = parts.at(0).trimmed().toInt(&okW);
    const int h = parts.at(1).trimmed().toInt(&okH);
    if (!okW || !okH || w < 2 || h < 2)
        return fallback;
    return QSize(w, h);
}

} // namespace

// ===========================================================================
//  FramePumpService — GUI-thread front-end
// ===========================================================================

FramePumpService::FramePumpService(ConfigManager* config, CaptureLocations* locations,
                                   QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_locations(locations)
{
    // A crash or a power cut mid-export leaves a reserved *.part behind. It is
    // invisible to the gallery, but nothing else would ever remove it. Only
    // files older than the threshold go: an export takes seconds, never ten
    // minutes, so a running one can never be swept away.
    if (m_locations)
        CapturePublisher::sweepStale(m_locations->clipsBaseRoot(), kStaleClipPartMaxAgeSecs);

    m_worker = new FramePumpWorker();   // no parent → we move it to m_thread
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &FramePumpWorker::onThreadStarted);
    // Release COM resources on their owning thread before reporting completion.
    connect(&m_thread, &QThread::finished, m_worker, &FramePumpWorker::onThreadFinished,
            Qt::DirectConnection);
    connect(&m_thread, &QThread::finished, this, &FramePumpService::shutdownFinished,
            Qt::QueuedConnection);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this, &FramePumpService::bufferStateChanged);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this, &FramePumpService::bufferStatusChanged);
    connect(&m_buffer, &ReplayBufferState::recordingStateChanged,
            this, &FramePumpService::recordingStateChanged);
    connect(&m_buffer, &ReplayBufferState::failed, this, &FramePumpService::failed);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this,
            [this](ReplayBufferState::State state, const QString&) {
                if (state == ReplayBufferState::Starting || state == ReplayBufferState::Stopped
                    || state == ReplayBufferState::Failed) {
                    m_borderState = CaptureBorder::Unknown;
                    m_borderDetail.clear();
                    emit captureBorderStateChanged();
                }
                if (state == ReplayBufferState::Stopped)
                    m_targetGame = {};
            });
    connect(m_worker, &FramePumpWorker::bufferStateChanged, this,
            [this](quint64 generation, ReplayBufferState::State state, const QString& reason) {
                m_buffer.confirm(generation, state, reason);
            });
    connect(m_worker, &FramePumpWorker::borderStateChanged, this,
            [this](quint64 generation, int state, const QString& detail) {
                // Only the live generation may speak for the border, same fencing rule
                // the buffer state uses.
                if (generation != m_buffer.generation())
                    return;
                const auto next = static_cast<CaptureBorder::State>(state);
                if (next == m_borderState && detail == m_borderDetail)
                    return;
                m_borderState = next;
                m_borderDetail = detail;
                emit captureBorderStateChanged();
            });
    connect(m_worker, &FramePumpWorker::restartRequested, this,
            [this](quint64 generation, const QString& reason) {
                if (generation != m_buffer.generation() || m_preparingForUpdate)
                    return;
                qInfo().noquote() << QStringLiteral("FramePump: %1 — rebuilding capture pipeline")
                                         .arg(reason);
                restartBuffer();
            });
    connect(m_worker, &FramePumpWorker::clipSaving, this, &FramePumpService::clipSaving);
    // There is one admitted replay export at a time. Immediate rejections carry
    // their own request id and must never overwrite the active export identity.
    connect(m_worker, &FramePumpWorker::clipSaved, this,
            [this](const QString& path, const QString& game, const QString& thumb, const QString& exe) {
                emit clipSaved(path, game, thumb, exe, m_activeReplayOperation);
            });
    connect(m_worker, &FramePumpWorker::clipFailed, this,
            [this](const QString& game, const QString& reason) {
                emit clipFailed(game, reason, m_activeReplayOperation);
            });
    connect(m_worker, &FramePumpWorker::saveRequestFinished, this, [this](quint64 requestId) {
        if (m_owners.finishSave(requestId)) {
            m_activeReplayOperation = 0;
            ownersChanged("save finished", requestId);
        }
    });
    connect(m_worker, &FramePumpWorker::hdrScreenshotReady, this,
            [this](quint64 generation, quint64 requestId, const QImage& image, const QString& game, const QString& exePath) {
                if (!m_owners.finishHdr(requestId))
                    return;
                const bool sameTarget = generation == m_buffer.generation();
                const auto operationId = std::exchange(m_activeHdrOperation, 0);
                ownersChanged("HDR finished", requestId);
                if (sameTarget)
                    emit hdrScreenshotReady(image, game, exePath, operationId);
                else
                    emit hdrScreenshotFailed(QStringLiteral("HDR screenshot cancelled because the capture target changed"), operationId);
            });
    connect(m_worker, &FramePumpWorker::hdrScreenshotFailed, this,
            [this](quint64, quint64 requestId, const QString& reason) {
                if (!m_owners.finishHdr(requestId))
                    return;
                const auto operationId = std::exchange(m_activeHdrOperation, 0);
                ownersChanged("HDR failed", requestId);
                emit hdrScreenshotFailed(reason, operationId);
            });
    connect(m_worker, &FramePumpWorker::exportBusyChanged, this, [this](bool busy) {
        if (m_exportBusy == busy)
            return;
        m_exportBusy = busy;
        emit exportBusyChanged(busy);
    });
    connect(m_worker, &FramePumpWorker::updateReady, this, [this](quint64 generation) {
        if (!m_preparingForUpdate || generation != m_buffer.generation())
            return;
        m_buffer.requestStop();
        emit updateReady();
    });

    // Always-on auto-arm (replay.auto, default true): a light poll of the foreground
    // window arms the buffer whenever a game is focused — no manual keypress needed.
    m_autoEnabled = m_config ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    m_ownerClock.start();
    if (m_config) {
        connect(m_config, &ConfigManager::valueChanged, this, [this](const QString& key) {
            if (key == ConfigKeys::CaptureMode || key == ConfigKeys::CaptureAllowedExecutables)
                enforceTargetPolicy();
        });
        connect(m_config, &ConfigManager::groupReset, this, &FramePumpService::enforceTargetPolicy);
    }
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(1500);
    connect(m_autoTimer, &QTimer::timeout, this, &FramePumpService::autoTick);
    m_autoTimer->start();

    m_thread.start();
}

void FramePumpService::ownersChanged(const char* reason, quint64 requestId)
{
    qInfo().noquote() << QStringLiteral("ReplayOwner[%1]: %2 auto=%3 manual=%4 hdr=%5 savePending=%6")
        .arg(requestId).arg(QLatin1String(reason))
        .arg(m_owners.owns(ReplayBufferOwners::Auto))
        .arg(m_owners.owns(ReplayBufferOwners::ManualSave))
        .arg(m_owners.owns(ReplayBufferOwners::HdrScreenshot)).arg(m_owners.savePending());
    if (!m_preparingForUpdate && !m_exitRequested && !m_owners.needsSession())
        stopBuffer();
}

void FramePumpService::saveReplay(const CaptureRequest& request)
{
    emit requestAccepted(request, CaptureRequest::Kind::Replay);
    const QString chain = request.tag();
    // Every exit below logs. Before this, the four early rejections emitted
    // clipFailed() and nothing else, so a save that died here left no trace of
    // the press at all - the exact shape of the reported "View/Back does
    // nothing" symptom.
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: accepted").arg(chain);
    const auto reject = [this, &chain, &request](const QString& game, const QString& reason) {
        qWarning().noquote() << QStringLiteral("ReplaySave[%1]: failed - %2").arg(chain, reason);
        emit clipFailed(game, reason, request.id);
    };
    if (m_preparingForUpdate) {
        reject(QStringLiteral("Replay"), QStringLiteral("Replay capture is paused for an update"));
        return;
    }
    if (m_exitRequested) {
        reject(QStringLiteral("Replay"), QStringLiteral("GameHQ is shutting down"));
        return;
    }
    if (m_owners.savePending()) {
        reject(QStringLiteral("Replay"), QStringLiteral("A replay save is already in progress"));
        return;
    }
    const auto current = GameDetector::current();
    if (current.isPlaynitePauseMenu) {
        if (!m_buffer.canSave()
            || !GameDetector::canRetainBehindPauseMenu(current, m_targetGame, m_config)) {
            reject(QStringLiteral("Replay"), QStringLiteral("No replay ready during game pause"));
            return;
        }
    } else if (!GameDetector::shouldCapture(current, m_config)) {
        reject(QStringLiteral("Replay"), QStringLiteral("No eligible foreground game for replay capture"));
        return;
    }
    const bool changeTarget = !current.isPlaynitePauseMenu
        && !GameDetector::sameCaptureTarget(current, m_targetGame);
    if (changeTarget && m_owners.owns(ReplayBufferOwners::HdrScreenshot)) {
        reject(QStringLiteral("Replay"), QStringLiteral("A screenshot of another window is still pending"));
        return;
    }
    const int idleSeconds = m_config
        ? m_config->value(ConfigKeys::ReplayManualIdleSeconds, 90).toInt() : 90;
    const quint64 requestId = m_owners.acquireManual(m_ownerClock.elapsed(), idleSeconds);
    ownersChanged("manual save requested", requestId);
    if (!m_buffer.startRequested() || changeTarget)
        startBuffer(current);
    if (!m_buffer.canSave()) {
        // A cold arm keeps its owner until the next save or bounded idle expiry.
        // No pending save is silently deferred until readiness.
        reject(m_buffer.gameName().isEmpty() ? QStringLiteral("Replay") : m_buffer.gameName(),
               m_buffer.saveRejection());
        return;
    }
    m_owners.beginSave(requestId);
    m_activeReplayOperation = request.id;
    ownersChanged("save dispatched", requestId);
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: armed generation=%2 owner=%3 game=%4")
                             .arg(chain).arg(m_buffer.generation()).arg(requestId)
                             .arg(m_buffer.gameName().isEmpty() ? QStringLiteral("?")
                                                                : m_buffer.gameName());
    const QString clipsBaseRoot = m_locations ? m_locations->clipsBaseRoot() : Paths::capturesRoot();
    if (!QMetaObject::invokeMethod(m_worker, "saveReplayOnWorker", Qt::QueuedConnection,
                                  Q_ARG(QString, clipsBaseRoot), Q_ARG(quint64, m_buffer.generation()),
                                  Q_ARG(quint64, requestId), Q_ARG(quint64, request.id))) {
        m_owners.finishSave(requestId);
        m_activeReplayOperation = 0;
        ownersChanged("save dispatch failed", requestId);
        reject(QStringLiteral("Replay"), QStringLiteral("Could not dispatch replay save"));
    }
}

void FramePumpService::captureHdrScreenshot(const ForegroundGame& target, quint64 operationId)
{
    if (m_exitRequested) {
        emit hdrScreenshotFailed(QStringLiteral("GameHQ is shutting down"), operationId);
        return;
    }
    if (m_preparingForUpdate) {
        emit hdrScreenshotFailed(QStringLiteral("screenshot capture is paused for an update"), operationId);
        return;
    }
    const auto current = GameDetector::current();
    if (current.isPlaynitePauseMenu || !GameDetector::shouldCapture(current, m_config)
        || !GameDetector::sameCaptureTarget(target, current)) {
        emit hdrScreenshotFailed(QStringLiteral("HDR screenshot cancelled because the capture target changed"), operationId);
        return;
    }
    const bool sameTarget = GameDetector::sameCaptureTarget(m_targetGame, current);
    if (m_owners.owns(ReplayBufferOwners::ManualSave) && m_targetGame.valid && !sameTarget) {
        emit hdrScreenshotFailed(QStringLiteral("HDR screenshot target differs from the active manual replay session"), operationId);
        return;
    }
    const quint64 requestId = m_owners.acquireHdr();
    if (!requestId) {
        emit hdrScreenshotFailed(QStringLiteral("an HDR screenshot is already pending"), operationId);
        return;
    }
    m_activeHdrOperation = operationId;
    ownersChanged("HDR requested", requestId);
    if (!m_buffer.startRequested() || !sameTarget)
        startBuffer(current);
    if (!m_buffer.startRequested()) {
        m_owners.finishHdr(requestId);
        m_activeHdrOperation = 0;
        ownersChanged("HDR arm failed", requestId);
        emit hdrScreenshotFailed(QStringLiteral("could not arm HDR capture on the foreground game"), operationId);
        return;
    }
    if (!QMetaObject::invokeMethod(m_worker, "captureScreenshotOnWorker", Qt::QueuedConnection,
                                  Q_ARG(quint64, m_buffer.generation()), Q_ARG(quint64, requestId))) {
        m_owners.finishHdr(requestId);
        m_activeHdrOperation = 0;
        ownersChanged("HDR dispatch failed", requestId);
        emit hdrScreenshotFailed(QStringLiteral("could not dispatch HDR capture"), operationId);
        return;
    }
    // Backstop: a capture that never produces a frame (stopped pump, fresh
    // session with nothing presented yet) must not hold the lease forever,
    // or every later press reports "already pending".
    constexpr int kHdrScreenshotTimeoutMs = 5000;
    QTimer::singleShot(kHdrScreenshotTimeoutMs, this, [this, requestId] {
        if (!m_owners.finishHdr(requestId))
            return;
        QMetaObject::invokeMethod(m_worker, "cancelHdrScreenshotOnWorker", Qt::QueuedConnection,
                                  Q_ARG(quint64, requestId));
        const auto operationId = std::exchange(m_activeHdrOperation, 0);
        ownersChanged("HDR timed out", requestId);
        emit hdrScreenshotFailed(QStringLiteral("No game frame available for screenshot"), operationId);
    });
}

void FramePumpService::prepareForUpdate()
{
    if (m_preparingForUpdate || m_exitRequested)
        return;
    m_preparingForUpdate = true;
    m_owners.setAuto(false);
    m_owners.releaseColdManual(); // explicit shutdown cancels an idle arm, never an export
    ownersChanged("update preparation");
    emit preparingForUpdateChanged(true);
    if (m_autoTimer)
        m_autoTimer->stop();
    if (m_exportBusy)
        emit updateWaitingForExport();
    QMetaObject::invokeMethod(m_worker, "prepareForUpdate", Qt::QueuedConnection,
                              Q_ARG(quint64, m_buffer.generation()));
}

void FramePumpService::cancelUpdatePreparation()
{
    if (!m_preparingForUpdate || m_exitRequested)
        return;
    m_preparingForUpdate = false;
    emit preparingForUpdateChanged(false);
    QMetaObject::invokeMethod(m_worker, "cancelUpdatePreparation", Qt::QueuedConnection);
    if (m_autoTimer && !m_autoTimer->isActive())
        m_autoTimer->start();
}

void FramePumpService::restartBuffer()
{
    m_autoEnabled = m_config ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    if (m_preparingForUpdate)
        return;
    if (!m_autoEnabled)
        m_owners.setAuto(false);
    ownersChanged("settings or capture restart", m_owners.manualToken());
    updateAutoTarget(GameDetector::current(), true);
}

FramePumpService::~FramePumpService()
{
    shutdownForExit();
    m_thread.wait();
    delete m_worker;
}

void FramePumpService::shutdownForExit()
{
    if (m_exitRequested)
        return;
    m_exitRequested = true;
    qInfo() << "FramePump: shutdown requested";
    m_autoTimer->stop();
    m_thread.requestInterruption();
    // FIFO dispatch lets admitted saves start before the terminal command.
    // Their completion publishes the clip and releases its segment lease.
    QMetaObject::invokeMethod(m_worker, &FramePumpWorker::shutdownForExit, Qt::QueuedConnection);
}

void FramePumpService::autoTick()
{
    if (m_preparingForUpdate || m_exitRequested)
        return;
    const quint64 manualToken = m_owners.manualToken();
    if (m_owners.expireIdle(m_ownerClock.elapsed()))
        ownersChanged("manual idle expired", manualToken);
    updateAutoTarget(GameDetector::current());
}

void FramePumpService::updateAutoTarget(const ForegroundGame& g, bool restartRequested)
{
    enforceTargetPolicy();
    if (restartRequested)
        m_restartPending = true;
    if (m_autoEnabled && m_owners.owns(ReplayBufferOwners::Auto) && m_buffer.startRequested()
        && GameDetector::canRetainBehindPauseMenu(g, m_targetGame, m_config)) {
        m_noGameTicks = 0;
        return;
    }
    const bool isGame = !g.isPlaynitePauseMenu && GameDetector::shouldCapture(g, m_config);
    if (g.valid && !g.isExcludedProcess) {
        emit foregroundGameDetected(g.gameName, g.executablePath,
                                    g.isGame || g.hasExternalIdentity);
        emit foregroundCaptureTarget(g.gameName, g.isGame || g.hasExternalIdentity, isGame);
    }
    if (m_autoEnabled && isGame) {
        m_noGameTicks = 0;
        if (!m_owners.owns(ReplayBufferOwners::Auto)) {
            m_owners.setAuto(true);
            ownersChanged("auto acquired");
        }
    } else {
        if (!g.valid || g.isExcludedProcess) {
            emit foregroundGameDetected(QString(), QString(), false);
            emit foregroundCaptureTarget(QString(), false, false);
        }
        if (m_owners.owns(ReplayBufferOwners::Auto) && ++m_noGameTicks >= 2) {
            m_owners.setAuto(false);
            ownersChanged("auto released after foreground grace");
        }
    }
    if (!isGame || !m_owners.needsSession())
        return;
    const bool sameTarget = GameDetector::sameCaptureTarget(m_targetGame, g);
    // An explicit request keeps its target. Defer a settings/recovery restart
    // until the approved game is back and in-flight frame requests have finished.
    if (m_owners.hasExplicitOwner()
        && (!m_restartPending || !sameTarget || m_owners.savePending()
            || m_owners.owns(ReplayBufferOwners::HdrScreenshot)))
        return;
    if (m_restartPending || !m_buffer.startRequested() || !sameTarget)
        startBuffer(g);
}

void FramePumpService::startBuffer(const ForegroundGame& g)
{
    if (m_preparingForUpdate || m_exitRequested || !m_owners.needsSession())
        return;
    m_restartPending = false;
    const QString gameName = g.gameName.isEmpty() ? QStringLiteral("Unknown Game") : g.gameName;
    const quint64 generation = m_buffer.requestStart(gameName);
    if (!GameDetector::shouldCapture(g, m_config)) {
        m_buffer.confirm(generation, ReplayBufferState::Failed,
                         QStringLiteral("No eligible foreground game for replay capture"));
        return;
    }
    m_targetGame = g;
    m_noGameTicks = 0;
    // Hand-edited config must not reach the encoder unchecked: bitrate is
    // multiplied by 1e6 into an unsigned, and segment/length drive the ring.
    const int fps  = qBound(1, m_config ? m_config->value(ConfigKeys::ReplayFps, 30).toInt() : 30, 240);
    const int mbps = qBound(1, m_config ? m_config->value(ConfigKeys::ReplayBitrateMbps, 14).toInt() : 14, 500);
    const int seg  = qBound(1, m_config ? m_config->value(ConfigKeys::ReplaySegmentSeconds, 5).toInt() : 5, 60);
    const int len  = qBound(5, m_config ? m_config->value(ConfigKeys::ReplayLengthSeconds, 300).toInt() : 300, 7200);
    const QSize res = parseResolution(
        m_config ? m_config->value(ConfigKeys::ReplayResolution,
                                   QStringLiteral("1920x1080")).toString()
                 : QStringLiteral("1920x1080"),
        QSize(1920, 1080));
    const AudioSourcePolicy::Resolution audio = AudioSourcePolicy::resolve(
        m_config ? m_config->value(ConfigKeys::AudioSource, QStringLiteral("auto")).toString()
                 : QStringLiteral("auto"),
        m_config ? m_config->value(ConfigKeys::AudioEnabled, true).toBool() : true,
        AudioCapture::processLoopbackSupported());
    // t24: hidden, default-on — see ConfigKeys::InternalCaptureExperimentalHdr.
    const bool hdrExperimentalEnabled = m_config
        ? m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                          ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool()
        : ConfigKeys::InternalCaptureExperimentalHdrDefault;
    const QString executablePath = g.executablePath;
    // Copy at dispatch, never read mutable GUI-thread config on the worker.
    const CaptureBorder::SessionPolicy borderPolicy(m_config
        ? m_config->value(ConfigKeys::CaptureHideBorder, true).toBool() : true);
    qInfo().noquote() << QStringLiteral("FramePump: start requested on %1 (audio: %2)")
                             .arg(gameName, AudioSourcePolicy::toConfigValue(audio.source));
    const bool queued = QMetaObject::invokeMethod(m_worker, "startPump", Qt::QueuedConnection,
                              Q_ARG(quint64, generation),
                              Q_ARG(qulonglong, qulonglong(reinterpret_cast<quintptr>(g.hwnd))),
                              Q_ARG(unsigned long, g.pid),
                              Q_ARG(int, res.width()), Q_ARG(int, res.height()),
                              Q_ARG(int, fps), Q_ARG(int, mbps), Q_ARG(int, seg),
                              Q_ARG(int, len), Q_ARG(QString, gameName),
                              Q_ARG(QString, executablePath),
                              Q_ARG(int, int(audio.source)),
                              Q_ARG(bool, hdrExperimentalEnabled),
                              Q_ARG(CaptureBorder::SessionPolicy, borderPolicy));
    if (!queued)
        m_buffer.confirm(generation, ReplayBufferState::Failed,
                         QStringLiteral("Could not dispatch replay startup"));
}

void FramePumpService::enforceTargetPolicy()
{
    if (m_preparingForUpdate || !m_targetGame.valid
        || GameDetector::shouldCapture(m_targetGame, m_config))
        return;
    m_owners.setAuto(false);
    m_owners.releaseColdManual();
    // An already admitted export owns its segment lease separately. Stop
    // capturing the revoked target immediately, even if that export is busy.
    stopBuffer();
}

void FramePumpService::stopBuffer()
{
    m_restartPending = false;
    if (m_buffer.state() == ReplayBufferState::Stopped)
        return;
    m_targetGame = {};
    const quint64 generation = m_buffer.requestStop();
    m_noGameTicks = 0;
    QMetaObject::invokeMethod(m_worker, "stopPumpForGeneration", Qt::QueuedConnection,
                              Q_ARG(quint64, generation));
}
