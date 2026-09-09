#pragma once

#include "capture/CaptureRequest.h"
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <atomic>

class QImage;
class ConfigManager;
class CaptureLocations;

// Grabs a screenshot of the foreground game and saves it as PNG under the
// effective screenshot root: <root>/<Game>/Screenshots/<timestamp>.png.
//
// SDR targets use the original GDI screen-DC BitBlt path. With experimental
// HDR enabled, capture() asks FramePumpService for a tone-mapped FP16 WGC frame
// and saveImage() feeds it through the same asynchronous encoding tail.
class ScreenshotService : public QObject
{
    Q_OBJECT
public:
    ScreenshotService(ConfigManager* config, CaptureLocations* locations,
                      QObject* parent = nullptr);
    ~ScreenshotService() override;
    bool busy() const { return m_pendingWrites.load() > 0; }

public slots:
    // Grab per capture.mode; emits exactly one result signal. The request is the
    // chain id every stage line carries, so a press that the mode gate silently
    // dropped is still visible in the log (docs/capture-engine.md).
    void capture(const CaptureRequest& request);
    // For callers with no press behind them (QML, internal retries).
    void capture() { capture(CaptureRequest::create(CaptureRequest::Source::Ui)); }
    // Save an already-grabbed image (e.g. a clip frame from the QML video
    // surface) as a screenshot under the given game, reusing the same encode +
    // feedback path as capture(). No foreground gating — the caller owns the
    // pixels already.
    void saveImage(const QImage& img, const QString& gameName,
                   const QString& executablePath = QString());
    void reportHdrCaptureFailure(const QString& reason) { emit failed(reason); }
    void prepareForUpdate();
    void cancelUpdatePreparation();

signals:
    void grabbed();                        // pixels are in hand — play shutter NOW
    void captured(const QString& filePath, const QString& gameName,
                  const QString& executablePath);
    void hdrCaptureRequested(qulonglong hwnd);
    void skipped(const QString& reason);   // gate said "not in a game"
    void failed(const QString& reason);    // grab or save error
    void updateReady();

private:
    // A 4K frame is ~33 MB of retained QImage, and the encode pool's queue is
    // unbounded, so a held-down capture button used to grow memory without
    // limit. Refuse new work past either limit and say so instead.
    static constexpr int kMaxPendingEncodes = 8;
    static constexpr qint64 kMaxPendingBytes = 256LL * 1024 * 1024;
    // Anything older than this cannot belong to a running encode.
    static constexpr qint64 kStalePendingMaxAgeSecs = 600;

    bool encodeBacklogFull() const;
    QImage grabRect(void* hwnd, int x, int y, int w, int h) const;
    // Shared tail of capture()/saveImage(): read format/quality on this thread,
    // then encode + write on a pool thread and emit captured()/failed().
    void encodeAndSave(const QImage& img, const QString& gameName,
                       const QString& executablePath);

    ConfigManager* m_config;
    CaptureLocations* m_locations;
    std::atomic_int m_pendingWrites{0};
    std::atomic_llong m_pendingBytes{0};
    std::atomic_bool m_updatePreparing{false};
    // Encode workers capture `this`, so they must never outlive the service.
    // A service-owned pool (declared last → destroyed first) makes the
    // destructor wait for in-flight encodes before any member goes away.
    QThreadPool m_encodePool;
};
