#include "storage/ThumbnailService.h"
#include "capture/ReplayExporter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QDir>
#include <QSaveFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace ThumbnailService
{

bool isUsableThumbnail(const QString& thumbnailPath)
{
    const QFileInfo info(thumbnailPath);
    if (!info.isFile() || info.size() <= 0)
        return false;

    QImageReader reader(thumbnailPath);
    reader.setDecideFormatFromContent(true);
    return reader.canRead();
}

bool isCurrentThumbnail(const QString& thumbnailPath, const QString& filePath,
                        const QString& type)
{
    if (!isUsableThumbnail(thumbnailPath))
        return false;
    if (type != QLatin1String("screenshot"))
        return true;
    const QFileInfo source(filePath);
    return !source.exists()
        || QFileInfo(thumbnailPath).lastModified() >= source.lastModified();
}

bool saveThumbnail(const QImage& image, const QString& thumbnailPath,
                   const QByteArray& format, int quality)
{
    if (image.isNull() || thumbnailPath.isEmpty())
        return false;

    QSaveFile output(thumbnailPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return false;

    bool written = false;
    {
        QImageWriter writer(&output, format);
        if (quality >= 0)
            writer.setQuality(quality);
        written = writer.write(image);
    }
    if (!written) {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}

QString ensureThumbnail(const QString& filePath, const QString& type,
                        const QString& thumbnailsDir)
{
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(filePath.toUtf8(), QCryptographicHash::Md5).toHex());
    const QString thumbPath = thumbnailsDir + QLatin1Char('/') + hash + QLatin1String(".jpg");

    if (isCurrentThumbnail(thumbPath, filePath, type))
        return thumbPath;
    if (QFileInfo::exists(thumbPath))
        QFile::remove(thumbPath);

    QDir().mkpath(thumbnailsDir);
    if (type == QLatin1String("video")) {
        // Early replay builds named previews after the clip instead of the
        // path hash. Reattach those orphaned previews before decoding again.
        const QString legacyThumb = thumbnailsDir + QLatin1Char('/')
                                    + QFileInfo(filePath).completeBaseName()
                                    + QStringLiteral("_clip.png");
        if (isUsableThumbnail(legacyThumb))
            return legacyThumb;
        if (QFileInfo::exists(legacyThumb))
            QFile::remove(legacyThumb);

        bool saved = false;
        QThread* worker = QThread::create([&] {
#ifdef Q_OS_WIN
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
            QImage frame;
            saved = ReplayExporter::grabThumbnail(filePath, frame)
                    && !frame.isNull() && saveThumbnail(frame, thumbPath, "JPG", 85);
#ifdef Q_OS_WIN
            if (SUCCEEDED(initialized))
                CoUninitialize();
#endif
        });
        worker->start();
        worker->wait();
        delete worker;
        return saved ? thumbPath : QString();
    }
    if (type != QLatin1String("screenshot"))
        return {};

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    // Decode downscaled to keep the scan pass fast on 4K screenshots.
    const QSize original = reader.size();
    if (original.isValid() && original.width() > 512)
        reader.setScaledSize(QSize(512, qMax(1, original.height() * 512 / original.width())));

    const QImage image = reader.read();
    if (image.isNull())
        return {};
    if (!saveThumbnail(image, thumbPath, "JPG", 85))
        return {};
    return thumbPath;
}

} // namespace ThumbnailService
