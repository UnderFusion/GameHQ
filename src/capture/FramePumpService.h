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
#include <QString>
#include <QThread>
#include <memory>

class ConfigManager;
class CaptureLocations;
class QImage;
class QTimer;
class ReplayExportTask;

// Auto-armed while a game is foreground (replay.auto, master switch in
// Settings → Replay). On start it captures the current foreground game window
// (gated by capture.mode, same as the screenshot path) via a free-threaded WGC
// Direct3D11CaptureFramePool. The MTA worker feeds rolling H.264 segments and,
// on HDR displays, exposes a tone-mapped BGRA8 frame for one-shot screenshots.
//
// All WinRT/D3D work runs on a dedicated MTA thread (FramePumpWorker): Qt's GUI
// thread is initialised as an STA, which is incompatible with RoInitialize(MTA) and
// the free-threaded frame pool. FramePumpService is the GUI-thread front-end.

// Runs on its own thread; owns all WGC/D3D state (opaque Pipeline, defined in the .cpp).
class FramePumpWorker : public QObject
{
    Q_OBJECT
public:
    explicit FramePumpWorker(QObject* parent = nullptr);
    ~FramePumpWorker() override;

public slots:
    void onThreadStarted();          // RoInitialize(MTA) on this worker thread
    void onThreadFinished();         // ensure stopped + RoUninitialize
    void startPump(quint64 generation, qulonglong hwnd, unsigned long pid, int encodeWidth, int encodeHeight,
                   int fps, int bitrateMbps, int segmentSeconds, int lengthSeconds,
                   const QString& gameName, const QString& executablePath,
                   bool audioEnabled,
                   bool hdrExperimentalEnabled = false,
                   CaptureBorder::SessionPolicy borderPolicy = CaptureBorder::SessionPolicy{}); // build pipeline + start polling/encoding
    void stopPump();                 // stop polling + tear down the pipeline
    void stopPumpForGeneration(quint64 generation);
    // chainId is the CaptureRequest id that started this save; it labels every
    // stage line. requestId stays the owners-table lease token.
    void saveReplayOnWorker(const QString& clipsBaseRoot, quint64 generation,
                            quint64 requestId, quint64 chainId);
    void captureScreenshotOnWorker(quint64 generation, quint64 requestId);
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
    // Fired the instant the ring is frozen (~1 s into the hold), before the
    // slower remux — thumbnailPath is a preview grabbed from the freshest
    // completed segment so the "saved" toast can show it right away.
    void clipSaving(const QString& gameName, const QString& thumbnailPath,
                    const QString& executablePath);
    void clipSaved(const QString& clipPath, const QString& gameName,
                   const QString& thumbnailPath, const QString& executablePath);
    void clipFailed(const QString& gameName, const QString& reason);
    void saveRequestFinished(quint64 requestId);
    void hdrScreenshotReady(quint64 generation, quint64 requestId, const QImage& image, const QString& gameName,
                            const QString& executablePath);
    void hdrScreenshotFailed(quint64 generation, quint64 requestId, const QString& reason);
    void exportBusyChanged(bool busy);
    void updateReady(quint64 generation);

private:
    friend class TestReplayThumbnail;
    struct Pipeline;                 // all WGC/D3D pointers + timer + fps state (.cpp)
    void teardown();                 // delete m_pipe (releases everything, reverse order)
    void finishExport();             // join before worker/apartment shutdown
    bool failStep(const char* step, long hr);
    void reportBufferState(ReplayBufferState::State state, const QString& reason = {});
    void checkReplayReadiness();     // low-frequency lifecycle check, outside poll()

    // startPump bring-up, one phase per step, in call order. Each reports its own
    // failure via failStep() and returns false; startPump owns the single
    // `delete pipe` cleanup, so no phase frees anything it did not create.
    bool createDevices(Pipeline* pipe);                        // D3D11 + WinRT bridge
    bool createCaptureItem(Pipeline* pipe, void* hwnd, int* outW, int* outH);
    void attachRecorder(Pipeline* pipe, unsigned long pid, int srcW, int srcH,
                        int encodeWidth, int encodeHeight, int fps, int bitrateMbps,
                        int segmentSeconds, int lengthSeconds, bool audioEnabled);
    bool createSession(Pipeline* pipe, void* hwnd, int srcW, int srcH,
                       bool hdrExperimentalEnabled,
                       const CaptureBorder::SessionPolicy& borderPolicy); // frame pool + session

    // saveReplayOnWorker stages, in call order.
    bool saveGuard(const QString& saveId);                     // preflight: pipe/ring/busy
    SegmentLease freezeRing(const QString& saveId);            // leased snapshot (empty = refused)
    static QString instantThumbnail(const QString& lastSegment, const QString& thumbPath,
                             const QString& saveId);
    void runExport(SegmentLease lease, const CapturePublisher::Reservation& reservation,
                   const QString& thumbPath,
                   const QString& game, const QString& exePath, const QString& saveId, quint64 requestId);

    Pipeline* m_pipe = nullptr;
    bool m_apartmentReady = false;
    quint64 m_pumpGeneration = 0;
    ReplayBufferState::State m_bufferState = ReplayBufferState::Stopped;
    bool m_exportBusy = false;       // one async clip export at a time
    std::unique_ptr<ReplayExportTask> m_exportTask;
    quint64 m_exportGeneration = 0;  // fences callbacks across export shutdown/replacement
    bool m_updatePreparing = false;
    quint64 m_updateGeneration = 0; // request may follow a GUI-side start rejection
    bool m_hdrScreenshotPending = false;
    quint64 m_hdrRequestId = 0;
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

public slots:
    // Share-hold: save the last N seconds as one clip. The request is the chain
    // id every stage line carries — accepted, armed, frozen, exporting,
    // published or failed (docs/capture-engine.md).
    void saveReplay(const CaptureRequest& request);
    // For callers with no press behind them (QML, internal retries).
    void saveReplay() { saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui)); }
    void captureHdrScreenshot(qulonglong hwnd, quint64 operationId = 0);
    // Replace the pipeline with new settings while preserving session owners.
    void restartBuffer();
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
    void hdrScreenshotReady(const QImage& image, const QString& gameName,
                            const QString& executablePath, quint64 operationId = 0);
    void hdrScreenshotFailed(const QString& reason, quint64 operationId = 0);
    void foregroundGameDetected(const QString& gameName, const QString& executablePath);
    // Rolling buffer armed/disarmed — drives the Settings "buffer state" row.
    void recordingStateChanged(bool active, const QString& gameName);
    void bufferStateChanged(ReplayBufferState::State state, const QString& gameName);
    void bufferStatusChanged();
    void captureBorderStateChanged();
    void exportBusyChanged(bool busy);
    void preparingForUpdateChanged(bool preparing);
    void updateWaitingForExport();
    void updateReady();

private slots:
    void autoTick();                 // poll the foreground game → arm/disarm the buffer

private:
    void startBuffer(bool rearm = false); // re-arm retains the owned target
    friend class ControllerClipE2ETest;
    void stopBuffer();               // disarm only when no owner needs the session
    void ownersChanged(const char* reason, quint64 requestId = 0);

    quint64 m_activeReplayOperation = 0;
    quint64 m_activeHdrOperation = 0;
    ConfigManager* m_config = nullptr;
    CaptureLocations* m_locations = nullptr;
    QThread m_thread;
    FramePumpWorker* m_worker = nullptr;   // lives on m_thread; deleted after it stops
    ReplayBufferState m_buffer;
    ReplayBufferOwners m_owners;
    QElapsedTimer m_ownerClock;
    ForegroundGame m_targetGame;
    bool m_exportBusy = false;
    bool m_preparingForUpdate = false;
    CaptureBorder::State m_borderState = CaptureBorder::Unknown;
    QString m_borderDetail;

    // Always-on auto-arm (replay.auto): while enabled, the buffer records whenever a
    // game is foreground (per capture.mode) — no manual arming needed.
    QTimer* m_autoTimer = nullptr;
    bool m_autoEnabled = true;
    int m_noGameTicks = 0;                 // grace period before auto-disarm
    qulonglong m_targetHwnd = 0;
};
