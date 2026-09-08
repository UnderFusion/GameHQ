#pragma once
#include "capture/CapturePublisher.h"
#include "capture/SegmentLease.h"

#include <QObject>
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
    void startPump(qulonglong hwnd, unsigned long pid, int encodeWidth, int encodeHeight,
                   int fps, int bitrateMbps, int segmentSeconds, int lengthSeconds,
                   const QString& gameName, const QString& executablePath,
                   bool audioEnabled,
                   bool hdrExperimentalEnabled = false); // build pipeline + start polling/encoding
    void stopPump();                 // stop polling + tear down the pipeline
    void saveReplayOnWorker(const QString& clipsBaseRoot);
    void captureScreenshotOnWorker();
    void prepareForUpdate();
    void cancelUpdatePreparation();

private slots:
    void poll();                     // one TryGetNextFrame tick

signals:
    void failed(const QString& reason);
    void restartRequested(const QString& reason);
    // Fired the instant the ring is frozen (~1 s into the hold), before the
    // slower remux — thumbnailPath is a preview grabbed from the freshest
    // completed segment so the "saved" toast can show it right away.
    void clipSaving(const QString& gameName, const QString& thumbnailPath,
                    const QString& executablePath);
    void clipSaved(const QString& clipPath, const QString& gameName,
                   const QString& thumbnailPath, const QString& executablePath);
    void clipFailed(const QString& gameName, const QString& reason);
    void hdrScreenshotReady(const QImage& image, const QString& gameName,
                            const QString& executablePath);
    void hdrScreenshotFailed(const QString& reason);
    void exportBusyChanged(bool busy);
    void updateReady();

private:
    struct Pipeline;                 // all WGC/D3D pointers + timer + fps state (.cpp)
    void teardown();                 // delete m_pipe (releases everything, reverse order)
    void finishExport();             // join before worker/apartment shutdown

    // startPump bring-up, one phase per step, in call order. Each reports its own
    // failure via failStep() and returns false; startPump owns the single
    // `delete pipe` cleanup, so no phase frees anything it did not create.
    bool createDevices(Pipeline* pipe);                        // D3D11 + WinRT bridge
    bool createCaptureItem(Pipeline* pipe, void* hwnd, int* outW, int* outH);
    void attachRecorder(Pipeline* pipe, unsigned long pid, int srcW, int srcH,
                        int encodeWidth, int encodeHeight, int fps, int bitrateMbps,
                        int segmentSeconds, int lengthSeconds, bool audioEnabled);
    bool createSession(Pipeline* pipe, void* hwnd, int srcW, int srcH,
                       bool hdrExperimentalEnabled); // frame pool + session

    // saveReplayOnWorker stages, in call order.
    bool saveGuard(const QString& saveId);                     // preflight: pipe/ring/busy
    SegmentLease freezeRing(const QString& saveId);            // leased snapshot (empty = refused)
    QString instantThumbnail(const QString& lastSegment, const QString& thumbPath,
                             const QString& saveId);
    void runExport(SegmentLease lease, const CapturePublisher::Reservation& reservation,
                   const QString& thumbPath, const QString& instantThumb,
                   const QString& game, const QString& exePath, const QString& saveId);

    Pipeline* m_pipe = nullptr;
    bool m_apartmentReady = false;
    bool m_exportBusy = false;       // one async clip export at a time
    std::unique_ptr<ReplayExportTask> m_exportTask;
    quint64 m_exportGeneration = 0;  // fences callbacks across export shutdown/replacement
    bool m_updatePreparing = false;
    bool m_hdrScreenshotPending = false;
};

// GUI-thread owner: constructs the worker on a dedicated thread and relays toggle().
class FramePumpService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool exportBusy READ exportBusy NOTIFY exportBusyChanged)
    Q_PROPERTY(bool preparingForUpdate READ preparingForUpdate NOTIFY preparingForUpdateChanged)
public:
    explicit FramePumpService(ConfigManager* config, CaptureLocations* locations,
                              QObject* parent = nullptr);
    ~FramePumpService() override;
    bool exportBusy() const { return m_exportBusy; }
    bool preparingForUpdate() const { return m_preparingForUpdate; }

public slots:
    void saveReplay();               // Share-hold: save the last N seconds as one clip
    void captureHdrScreenshot(qulonglong hwnd);
    // Replay settings changed (fps/resolution/length): disarm a running
    // buffer so the auto-arm tick re-arms it with the new parameters.
    void restartBuffer();
    void prepareForUpdate();
    void cancelUpdatePreparation();

signals:
    void failed(const QString& reason);
    void clipSaving(const QString& gameName, const QString& thumbnailPath,
                    const QString& executablePath);   // ring frozen - instant feedback
    void clipSaved(const QString& clipPath, const QString& gameName,
                   const QString& thumbnailPath, const QString& executablePath);
    void clipFailed(const QString& gameName, const QString& reason);
    void hdrScreenshotReady(const QImage& image, const QString& gameName,
                            const QString& executablePath);
    void hdrScreenshotFailed(const QString& reason);
    void foregroundGameDetected(const QString& gameName, const QString& executablePath);
    // Rolling buffer armed/disarmed — drives the Settings "buffer state" row.
    void recordingStateChanged(bool active, const QString& gameName);
    void exportBusyChanged(bool busy);
    void preparingForUpdateChanged(bool preparing);
    void updateWaitingForExport();
    void updateReady();

private slots:
    void autoTick();                 // poll the foreground game → arm/disarm the buffer

private:
    void startBuffer();              // arm on the current foreground game (if it is a game)
    void stopBuffer();               // disarm

    ConfigManager* m_config = nullptr;
    CaptureLocations* m_locations = nullptr;
    QThread m_thread;
    FramePumpWorker* m_worker = nullptr;   // lives on m_thread; deleted after it stops
    bool m_running = false;
    bool m_exportBusy = false;
    bool m_preparingForUpdate = false;
    bool m_stopAfterHdrScreenshot = false;

    // Always-on auto-arm (replay.auto): while enabled, the buffer records whenever a
    // game is foreground (per capture.mode) — no manual arming needed.
    QTimer* m_autoTimer = nullptr;
    bool m_autoEnabled = true;
    int m_noGameTicks = 0;                 // grace period before auto-disarm
    qulonglong m_targetHwnd = 0;
};
