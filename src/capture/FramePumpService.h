#pragma once

#include "capture/CaptureRequest.h"
#include "capture/CaptureBorderState.h"
#include "capture/CapturePublisher.h"
#include "capture/SegmentLease.h"
#include "capture/ReplayBufferState.h"
#include "capture/ReplayBufferOwners.h"
#include "games/GameDetector.h"

#include <QObject>
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QThread>
#include <memory>

class ConfigManager;
class CaptureLocations;
class QImage;
class QTimer;
class ReplayExportTask;

// Screenshots and auto-armed replay share the selected game window. The MTA
// worker crops each WGC frame to the client area and tone-maps HDR before
// handing pixels to the screenshot readback or rolling replay recorder.
//
// All WinRT/D3D work runs on a dedicated MTA thread (FramePumpWorker): Qt's GUI
// thread is initialised as an STA, which is incompatible with RoInitialize(MTA) and
// the free-threaded frame pool. FramePumpService is the GUI-thread front-end.

// Runs on its own thread; owns all WGC/D3D state (opaque Pipeline, defined in FramePumpPipeline.h).
class FramePumpWorker : public QObject
{
    Q_OBJECT
public:
    explicit FramePumpWorker(QObject* parent = nullptr);
    ~FramePumpWorker() override;

public slots:
    void onThreadStarted();          // RoInitialize(MTA) on this worker thread
    void onThreadFinished();         // ensure stopped + RoUninitialize
    void shutdownForExit();
    void startPump(quint64 generation, qulonglong hwnd, unsigned long pid, int encodeWidth, int encodeHeight,
                   int fps, int bitrateMbps, int segmentSeconds, int lengthSeconds,
                   const QString& gameName, const QString& executablePath,
                   int audioSource,          // AudioSourcePolicy::Source
                   bool hdrExperimentalEnabled, CaptureBorder::SessionPolicy borderPolicy,
                   quint64 processStarted, bool recordReplay, const QString& replayCacheKey);
    void stopPump();                 // stop polling + tear down the pipeline
    void stopPumpForGeneration(quint64 generation);
    // chainId is the CaptureRequest id that started this save; it labels every
    // stage line. requestId stays the owners-table lease token.
    void saveReplayOnWorker(const QString& clipsBaseRoot, quint64 generation,
                            quint64 requestId, quint64 chainId);
    void captureScreenshotOnWorker(quint64 generation, quint64 requestId);
    void cancelScreenshotOnWorker(quint64 requestId);
    // #41: Windows mouse pointer in screenshots and in the replay. The live
    // session follows the replay value (the screenshot value when it records
    // no replay); a screenshot needing the other value uses a one-shot session.
    void setCursorPolicy(bool screenshotCursor, bool replayCursor);
    void prepareForUpdate(quint64 generation);
    void cancelUpdatePreparation();

private slots:
    void poll();                     // one TryGetNextFrame tick

signals:
    void bufferStateChanged(quint64 generation, ReplayBufferState::State state, const QString& reason);
    // Honest capture-border verdict for the session just built (CaptureBorder::State),
    // with the diagnostic detail behind it. Never derived from the setter HRESULT alone.
    void borderStateChanged(quint64 generation, int state, const QString& detail);
    void restartRequested(quint64 generation, const QString& reason);
    void capturePausedChanged(quint64 generation, bool paused);
    void captureTargetLost(quint64 generation);
    // Fired the instant the ring is frozen (~1 s into the hold), before the
    // slower remux — thumbnailPath is a preview grabbed from the freshest
    // completed segment so the "saved" toast can show it right away.
    void clipSaving(const QString& gameName, const QString& thumbnailPath,
                    const QString& executablePath);
    void clipSaved(const QString& clipPath, const QString& gameName,
                   const QString& thumbnailPath, const QString& executablePath);
    void clipFailed(const QString& gameName, const QString& reason);
    void saveRequestFinished(quint64 requestId);
    void windowScreenshotReady(quint64 generation, quint64 requestId, const QImage& image, const QString& gameName,
                            const QString& executablePath);
    void windowScreenshotFailed(quint64 generation, quint64 requestId, const QString& reason);
    void exportBusyChanged(bool busy);
    void updateReady(quint64 generation);

private:
    friend class TestReplayThumbnail;
    friend class CaptureAcknowledgementTest;
    struct Pipeline;                 // all WGC/D3D pointers + timer + fps state (FramePumpPipeline.h)
    void teardown();                 // delete m_pipe (releases everything, reverse order)
    void finishExport();             // join before worker/apartment shutdown
    bool failStep(const char* step, long hr);
    void reportBufferState(ReplayBufferState::State state, const QString& reason = {});
    void checkReplayReadiness();     // low-frequency lifecycle check, outside poll()
    void completeScreenshot();
    bool wantedSessionCursor(const Pipeline* pipe) const;
    void applySessionCursor(Pipeline* pipe);
    bool screenshotNeedsStillCapture() const;
    bool startStillCapture(quint64 requestId, QString* reason);
    void pollStillCapture();
    void finishStillCapture(const QImage& image, const QString& failure);

    // startPump bring-up, one phase per step, in call order. Each reports its own
    // failure via failStep() and returns false; startPump owns the single
    // cleanup of the half-built pipe, so no phase frees anything it did not create.
    bool createDevices(Pipeline* pipe);                        // D3D11 + WinRT bridge
    bool createCaptureItem(Pipeline* pipe, void* hwnd, unsigned long pid, int* outW, int* outH);
    void attachRecorder(Pipeline* pipe, unsigned long pid, int srcW, int srcH,
                        int encodeWidth, int encodeHeight, int fps, int bitrateMbps,
                        int segmentSeconds, int lengthSeconds, int audioSource);
    bool createSession(Pipeline* pipe, void* hwnd, int srcW, int srcH,
                       bool hdrExperimentalEnabled,
                       const CaptureBorder::SessionPolicy& borderPolicy); // frame pool + session

    // saveReplayOnWorker stages, in call order.
    bool saveGuard(const QString& saveId);                     // preflight: pipe/ring/busy
    SegmentLease freezeRing(const QString& saveId);            // leased snapshot (empty = refused)
    void runExport(SegmentLease lease, const CapturePublisher::Reservation& reservation,
                   const QString& thumbPath,
                   const QString& game, const QString& exePath, const QString& saveId,
                   quint64 requestId, bool requireAudio);

    Pipeline* m_pipe = nullptr;
    bool m_apartmentReady = false;
    // Media Foundation stays started for the worker's lifetime. Starting and
    // shutting it down around every recorder left two idle thread pools behind
    // per capture restart with hardware encoders (#39).
    bool m_mfHeld = false;
    quint64 m_pumpGeneration = 0;
    ReplayBufferState::State m_bufferState = ReplayBufferState::Stopped;
    bool m_exportBusy = false;       // one async clip export at a time
    std::unique_ptr<ReplayExportTask> m_exportTask;
    quint64 m_exportGeneration = 0;  // fences callbacks across export shutdown/replacement
    bool m_updatePreparing = false;
    quint64 m_updateGeneration = 0; // request may follow a GUI-side start rejection
    bool m_screenshotPending = false;
    quint64 m_screenshotRequestId = 0;
    QElapsedTimer m_screenshotPendingClock;
    bool m_screenshotCursor = false;  // ConfigKeys::CaptureScreenshotCursor default
    bool m_replayCursor = true;       // ConfigKeys::ReplayCursor default
    bool m_cursorUnsupportedLogged = false;
    // Runtime audio read failures that already forced a re-arm, in a short
    // window; past the limit the buffer fails instead of re-arming forever.
    QString m_audioFailure;          // reason while the current Failed state is an audio failure
    ReplayBufferState::AudioRearmBudget m_audioRearmBudget;
    // Same bound for a frame pool that keeps failing after re-arms.
    ReplayBufferState::AudioRearmBudget m_captureRearmBudget;
};

// GUI-thread owner: constructs the worker on a dedicated thread and relays toggle().
class FramePumpService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool exportBusy READ exportBusy NOTIFY exportBusyChanged)
    Q_PROPERTY(bool preparingForUpdate READ preparingForUpdate NOTIFY preparingForUpdateChanged)
    Q_PROPERTY(ReplayBufferState::State bufferState READ bufferState NOTIFY bufferStatusChanged)
    // Windows API verdict for the current session (CaptureBorder::State).
    Q_PROPERTY(int captureBorderState READ captureBorderState NOTIFY captureBorderStateChanged)
    Q_PROPERTY(QString captureBorderDetail READ captureBorderDetail NOTIFY captureBorderStateChanged)
public:
    explicit FramePumpService(ConfigManager* config, CaptureLocations* locations,
                              QObject* parent = nullptr);
    ~FramePumpService() override;
    bool exportBusy() const { return m_exportBusy; }
    bool preparingForUpdate() const { return m_preparingForUpdate; }
    ReplayBufferState::State bufferState() const { return m_buffer.state(); }
    int captureBorderState() const { return int(m_borderState); }
    QString captureBorderDetail() const { return m_borderDetail; }
    QList<ForegroundGame> captureTargets();
    bool hasPinnedTarget() const { return m_pinnedTarget.valid; }
    void pinTarget(const ForegroundGame& target);
    void useAutomaticCapture();

public slots:
    // Share-hold: save the last N seconds as one clip. The request is the chain
    // id every stage line carries — accepted, armed, frozen, exporting,
    // published or failed (docs/capture-engine.md).
    void saveReplay(const CaptureRequest& request);
    // For callers with no press behind them (QML, internal retries).
    void saveReplay() { saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui)); }
    void captureWindowScreenshot(quint64 operationId);
    // Replace the pipeline with new settings while preserving session owners.
    void restartBuffer();
    void shutdownForExit();
    void prepareForUpdate();
    void cancelUpdatePreparation();

signals:
    // Receipt only: the request may still be rejected by a later gate.
    void requestAccepted(const CaptureRequest& request, CaptureRequest::Kind kind);
    void failed(const QString& reason);
    void clipSaving(const QString& gameName, const QString& thumbnailPath,
                    const QString& executablePath);   // ring frozen - instant feedback
    void clipSaved(const QString& clipPath, const QString& gameName,
                   const QString& thumbnailPath, const QString& executablePath, quint64 operationId = 0);
    void clipFailed(const QString& gameName, const QString& reason, quint64 operationId = 0);
    void windowScreenshotReady(const QImage& image, const QString& gameName,
                            const QString& executablePath, quint64 operationId = 0);
    void windowScreenshotFailed(const QString& reason, quint64 operationId = 0);
    void windowScreenshotSkipped(const QString& reason, quint64 operationId);
    // Every valid, non-excluded foreground window. `recognizedGame` says
    // whether it passes the game heuristic or has an external game identity;
    // only such a window may become a new library game row.
    void foregroundGameDetected(const QString& gameName, const QString& executablePath,
                                bool recognizedGame);
    // The selected capture target and its state, independently of focus.
    void captureTargetChanged(const QString& gameName, bool recognizedGame,
                              bool captureEligible, bool background, bool pinned, bool paused);
    void selectedBufferStateChanged(int state);
    // Rolling buffer armed/disarmed — drives the Settings "buffer state" row.
    void recordingStateChanged(bool active, const QString& gameName);
    void bufferStateChanged(ReplayBufferState::State state, const QString& gameName);
    void bufferStatusChanged();
    void captureBorderStateChanged();
    void exportBusyChanged(bool busy);
    void preparingForUpdateChanged(bool preparing);
    void updateWaitingForExport();
    void updateReady();
    void shutdownFinished();

private slots:
    void autoTick();                 // poll the foreground game → arm/disarm the buffer

private:
    void startBuffer(const ForegroundGame& target, bool recordReplay = true);
    friend class ControllerClipE2ETest;
    friend class CaptureAcknowledgementTest;
    void updateAutoTarget(const ForegroundGame& current, bool restartRequested = false);
    ForegroundGame selectTarget(const ForegroundGame& current);
    bool backgroundEnabled() const;
    void clearCaptureTargets();
    void publishCaptureTarget();
    void enforceTargetPolicy();
    void publishCursorPolicy();
    void stopBuffer();
    void ownersChanged(const char* reason, quint64 requestId = 0);

    quint64 m_activeReplayOperation = 0;
    quint64 m_activeScreenshotOperation = 0;
    ConfigManager* m_config = nullptr;
    CaptureLocations* m_locations = nullptr;
    QThread m_thread;
    FramePumpWorker* m_worker = nullptr;   // lives on m_thread; deleted after it stops
    ReplayBufferState m_buffer;
    ReplayBufferOwners m_owners;
    QElapsedTimer m_ownerClock;
    ForegroundGame m_targetGame;
    ForegroundGame m_selectedTarget;
    ForegroundGame m_pinnedTarget;
    ForegroundGame m_lastFocusedTarget;
    QList<ForegroundGame> m_recentTargets;
    bool m_selectedInBackground = false;
    bool m_targetHasReplay = false;
    bool m_capturePaused = false;
    bool m_exportBusy = false;
    bool m_preparingForUpdate = false;
    bool m_exitRequested = false;
    CaptureBorder::State m_borderState = CaptureBorder::Unknown;
    QString m_borderDetail;

    // Auto-armed replay follows the selected target while ReplayAuto is on.
    QTimer* m_autoTimer = nullptr;
    bool m_autoEnabled = true;
    bool m_restartPending = false;
    int m_noGameTicks = 0;                 // grace period before auto-disarm
};
