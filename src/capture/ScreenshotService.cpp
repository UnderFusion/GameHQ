#include "capture/ScreenshotService.h"

#include "capture/CapturePublisher.h"
#include "capture/CaptureUtil.h"
#include "capture/HdrCapabilities.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/CaptureLocations.h"
#include "core/GameIdentity.h"
#include "games/GameDetector.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QRunnable>
#include <QThreadPool>

#include <windows.h>

ScreenshotService::ScreenshotService(ConfigManager* config, CaptureLocations* locations,
                                     QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_locations(locations)
{
    m_encodePool.setMaxThreadCount(2);
    // Unfinished captures from a crash or a power cut are invisible to the
    // scanner but would otherwise stay on disk forever.
    if (m_locations)
        CapturePublisher::sweepStale(m_locations->screenshotsBaseRoot(), kStalePendingMaxAgeSecs);
}

// Checked before the grab: refusing early costs the user a frame, while
// accepting would cost another full-resolution image in memory.
bool ScreenshotService::encodeBacklogFull() const
{
    return m_pendingWrites.load() >= kMaxPendingEncodes
        || m_pendingBytes.load() >= kMaxPendingBytes;
}

ScreenshotService::~ScreenshotService()
{
    // ~QThreadPool would wait too, but do it explicitly while the full object
    // is still alive: workers touch m_pendingWrites and emit signals.
    m_encodePool.waitForDone();
}

void ScreenshotService::capture(const CaptureRequest& request)
{
    const QString chain = request.tag();
    // Logged before any gate, so "I pressed it and nothing happened" always has
    // a matching line naming the reason.
    qInfo().noquote() << QStringLiteral("Screenshot[%1]: accepted").arg(chain);
    const auto reject = [this, &chain](const QString& reason) {
        qInfo().noquote() << QStringLiteral("Screenshot[%1]: skipped - %2").arg(chain, reason);
        emit skipped(reason);
    };
    if (m_updatePreparing.load()) {
        reject(QStringLiteral("screenshot capture is paused for an update"));
        return;
    }
    if (encodeBacklogFull()) {
        reject(QStringLiteral("still saving the previous screenshots"));
        return;
    }
    const QString mode = m_config
        ? m_config->value(ConfigKeys::CaptureMode,
                          QStringLiteral("only_in_games")).toString()
        : QStringLiteral("only_in_games");

    const ForegroundGame g = GameDetector::current();
    if (!GameDetector::shouldCapture(g, mode)) {
        reject(QStringLiteral("foreground window is not a game (mode=%1, process=%2)")
                   .arg(mode, g.processName.isEmpty() ? QStringLiteral("?") : g.processName));
        return;
    }

    const bool hdrExperimentalEnabled = m_config
        && m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                           ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool();
    if (hdrExperimentalEnabled) {
        const capture::HdrOutputInfo output =
            capture::HdrCapabilities::forWindow(static_cast<HWND>(g.hwnd));
        if (output.valid && output.hdrActive) {
            qInfo().noquote() << QStringLiteral(
                "Screenshot[%1]: HDR target detected (%2) - requesting tone-mapped WGC frame")
                                     .arg(chain, output.describe());
            emit hdrCaptureRequested(
                qulonglong(reinterpret_cast<quintptr>(g.hwnd)));
            return;
        }
    }

    QElapsedTimer t;
    t.start();
    const QImage img = grabRect(g.hwnd, g.x, g.y, g.w, g.h);
    const qint64 grabMs = t.elapsed();
    if (img.isNull()) {
        const QString reason = QStringLiteral("GDI grab returned no pixels");
        qWarning().noquote() << QStringLiteral("Screenshot[%1]: failed - %2").arg(chain, reason);
        emit failed(reason);
        return;
    }

    // The pixels are captured. Fire instant feedback NOW (the shutter sound) and
    // hand the slow PNG encode + disk write to a worker thread, so pressing
    // Share never freezes the UI/game overlay (a 4K frame took ~2.7 s inline).
    emit grabbed();

    const QString gameName = g.gameName.isEmpty() ? QStringLiteral("Unknown Game")
                                                  : g.gameName;
    qInfo().noquote() << QStringLiteral(
                             "Screenshot[%1]: grabbed %2x%3 in %4ms - encoding in background")
                             .arg(chain).arg(img.width()).arg(img.height()).arg(grabMs);
    encodeAndSave(img, gameName, g.executablePath);
}

// Save a caller-supplied image (a clip frame from the QML video surface) as a
// screenshot. Same feedback + encode path as capture(), minus the GDI grab and
// foreground gate — the caller already holds the pixels for a specific game.
void ScreenshotService::saveImage(const QImage& img, const QString& gameName,
                                  const QString& executablePath)
{
    if (m_updatePreparing.load()) {
        emit skipped(QStringLiteral("screenshot capture is paused for an update"));
        return;
    }
    if (img.isNull()) {
        emit failed(QStringLiteral("no video frame available to save"));
        return;
    }
    if (encodeBacklogFull()) {
        emit skipped(QStringLiteral("still saving the previous screenshots"));
        return;
    }
    emit grabbed();
    const QString game = gameName.isEmpty() ? QStringLiteral("Unknown Game") : gameName;
    qInfo() << "Frame grab:" << img.width() << "x" << img.height()
            << "for" << game << "— encoding in background";
    encodeAndSave(img, game, executablePath);
}

void ScreenshotService::encodeAndSave(const QImage& img, const QString& gameName,
                                      const QString& executablePath)
{
    const QString dir = m_locations->screenshotDir(gameName);
    // Read format/quality on the calling thread (ConfigManager is not meant for
    // concurrent access) and hand plain values to the worker.
    const bool jpeg = m_config
        && m_config->value(ConfigKeys::CaptureScreenshotFormat, QStringLiteral("png"))
               .toString().compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0;
    const QString ext = jpeg ? QStringLiteral(".jpg") : QStringLiteral(".png");
    const int jpegQuality = m_config
        ? qBound(1, m_config->value(ConfigKeys::CaptureJpegQuality, 90).toInt(), 100)
        : 90;

    const qint64 imageBytes = img.sizeInBytes();
    ++m_pendingWrites;
    m_pendingBytes += imageBytes;
    m_encodePool.start(QRunnable::create(
        [this, img, gameName, executablePath, dir, jpeg, ext, jpegQuality, imageBytes]() {
        struct Completion {
            ScreenshotService *service;
            qint64 bytes;
            ~Completion() {
                service->m_pendingBytes -= bytes;
                if (--service->m_pendingWrites == 0 && service->m_updatePreparing.load())
                    QMetaObject::invokeMethod(service, "updateReady", Qt::QueuedConnection);
            }
        } completion{this, imageBytes};
        QElapsedTimer et;
        et.start();
        if (!QDir().mkpath(dir)) {
            emit failed(QStringLiteral("could not create ") + dir);
            return;
        }

        // Take the timestamped name by creating the .part file exclusively, so
        // the other encoder thread cannot be handed the same one.
        const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
            dir, QStringLiteral("yyyy-MM-dd_HH-mm-ss"), ext);
        if (!reservation.isValid()) {
            emit failed(QStringLiteral("could not reserve a screenshot name in ") + dir);
            return;
        }

        // Encode into the .part file. The scanner ignores that extension, so a
        // slow, failed or interrupted encode is never visible as a capture.
        {
            QImageWriter writer(reservation.pendingPath, jpeg ? "JPG" : "PNG");
            if (jpeg)
                writer.setQuality(jpegQuality);
            else
                writer.setCompression(1);   // fast zlib level — encode speed over file size
            if (!writer.write(img)) {
                const QString reason = writer.errorString();
                CapturePublisher::discard(reservation);
                emit failed(QStringLiteral("could not write ") + reservation.finalPath
                            + QStringLiteral(": ") + reason);
                return;
            }
        }

        // QImageWriter keeps its output file open until destruction. Windows
        // cannot atomically rename that open file, so publish only after the
        // writer has left the scope above.
        QString publishError;
        if (!CapturePublisher::publish(reservation, &publishError)) {
            // The .part stays behind on purpose: the pixels are still in it and
            // the startup sweep will clear it if nothing ever claims it.
            emit failed(QStringLiteral("could not publish ") + reservation.finalPath
                        + QStringLiteral(": ") + publishError);
            return;
        }

        qInfo() << "Screenshot: saved" << reservation.finalPath
                << "(" << img.width() << "x" << img.height()
                << ") encode+write" << et.elapsed() << "ms";
        emit captured(reservation.finalPath, gameName, executablePath);
    }));
}

void ScreenshotService::prepareForUpdate()
{
    m_updatePreparing.store(true);
    if (!busy())
        emit updateReady();
}

void ScreenshotService::cancelUpdatePreparation()
{
    m_updatePreparing.store(false);
}

// Screen-DC BitBlt of the window's screen rectangle. CAPTUREBLT pulls in
// layered windows; whatever is composited on that region of the monitor is
// grabbed (correct for a foreground fullscreen/borderless game).
QImage ScreenshotService::grabRect(void* hwndPtr, int x, int y, int w, int h) const
{
    Q_UNUSED(hwndPtr);
    if (w <= 0 || h <= 0)
        return {};

    HDC screen = GetDC(nullptr);
    if (!screen)
        return {};

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    QImage img;

    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT)) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            QImage buffer(w, h, QImage::Format_RGB32);   // 0xffRRGGBB == BGRA in memory
            if (GetDIBits(mem, bmp, 0, h, buffer.bits(), &bi, DIB_RGB_COLORS))
                img = buffer.copy();
        }
        SelectObject(mem, old);
    }

    if (bmp)
        DeleteObject(bmp);
    if (mem)
        DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}
