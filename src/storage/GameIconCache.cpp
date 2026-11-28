#include "storage/GameIconCache.h"

#include "config/Paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QXmlStreamReader>

namespace {

// MSIX/Appx titles (Xbox app installs under <drive>\XboxGames\<title>\Content)
// ship an executable with no embedded icon resource, so the shell only ever
// hands back the generic application icon. The real artwork lives in the
// package manifest, which sits at the package root next to the executable.
//
// Two manifest flavours matter here. Store/MSIX packages carry
// AppxManifest.xml; GDK titles installed by the Xbox app carry
// MicrosoftGame.config instead, which declares the same logos as attributes on
// its <ShellVisuals> element. Only looking for the Appx one is why XboxGames
// installs kept falling through to the generic .exe glyph.
QString packageManifestFor(const QString& executablePath)
{
    static const QStringList kManifestNames{
        QStringLiteral("MicrosoftGame.config"),
        QStringLiteral("AppxManifest.xml"),
    };

    QDir dir = QFileInfo(executablePath).absoluteDir();
    for (int depth = 0; depth < 4; ++depth) {
        for (const QString& name : kManifestNames) {
            const QString candidate = dir.filePath(name);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
        if (!dir.cdUp())
            break;
    }
    return {};
}

// Logo references in preferred order: the largest square tile first, because
// it is the one that carries the actual key art rather than a stripped glyph.
QStringList manifestLogoReferences(const QString& manifestPath)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    static const QStringList wanted{
        QStringLiteral("Square480x480Logo"),   // GDK <ShellVisuals>
        QStringLiteral("Square310x310Logo"),
        QStringLiteral("Square150x150Logo"),
        QStringLiteral("Square44x44Logo"),
        QStringLiteral("Logo"),
    };
    QStringList byPriority(wanted.size(), QString());

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        for (const QXmlStreamAttribute& attr : xml.attributes()) {
            const int rank = wanted.indexOf(attr.name().toString());
            if (rank >= 0 && byPriority.at(rank).isEmpty())
                byPriority[rank] = attr.value().toString();
        }
        // <Properties><Logo>Assets\StoreLogo.png</Logo></Properties> is an
        // element, not an attribute.
        if (xml.name() == QLatin1String("Logo")) {
            const QString text = xml.readElementText();
            const int rank = wanted.indexOf(QStringLiteral("Logo"));
            if (!text.isEmpty() && byPriority.at(rank).isEmpty())
                byPriority[rank] = text;
        }
    }

    QStringList references;
    for (const QString& reference : byPriority) {
        if (!reference.isEmpty())
            references.append(reference);
    }
    return references;
}

// Manifests name a logical asset ("Assets\Square150x150Logo.png"); what is on
// disk are its qualified variants ("Square150x150Logo.scale-200.png"). Fall
// back to the fattest matching variant, which is the highest-resolution one.
QString resolveManifestAsset(const QString& manifestDir, const QString& reference)
{
    QString relative = reference;
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QFileInfo direct(QDir(manifestDir).filePath(relative));
    if (direct.isFile())
        return direct.absoluteFilePath();

    QDir assetDir(direct.absolutePath());
    if (!assetDir.exists())
        return {};

    const QString suffix = direct.suffix().isEmpty() ? QStringLiteral("png") : direct.suffix();
    const QStringList variants = assetDir.entryList(
        {direct.completeBaseName() + QStringLiteral(".*.") + suffix}, QDir::Files);

    QString best;
    qint64 bestSize = -1;
    for (const QString& variant : variants) {
        const QFileInfo info(assetDir.filePath(variant));
        if (info.size() > bestSize) {
            bestSize = info.size();
            best = info.absoluteFilePath();
        }
    }
    return best;
}

QPixmap packageLogoPixmap(const QString& executablePath)
{
    const QString manifestPath = packageManifestFor(executablePath);
    if (manifestPath.isEmpty()) {
        // Not a packaged title, or the manifest sits deeper than the walk goes.
        // Normal for plain Win32 games, so this stays at debug volume.
        qInfo() << "GameIconCache: no package manifest near" << executablePath;
        return {};
    }

    const QStringList references = manifestLogoReferences(manifestPath);
    if (references.isEmpty()) {
        qWarning() << "GameIconCache: manifest declares no logo" << manifestPath;
        return {};
    }

    const QString manifestDir = QFileInfo(manifestPath).absolutePath();
    for (const QString& reference : references) {
        const QString assetPath = resolveManifestAsset(manifestDir, reference);
        if (assetPath.isEmpty())
            continue;
        QImage image(assetPath);
        if (image.isNull())
            continue;
        if (image.width() > 64 || image.height() > 64)
            image = image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        qInfo() << "GameIconCache: using package logo" << assetPath << "from" << manifestPath;
        return QPixmap::fromImage(image);
    }
    qWarning() << "GameIconCache: no logo asset on disk for" << references << "declared by"
               << manifestPath;
    return {};
}

// The extractor's format version. It keys the on-disk cache below and is also
// persisted next to every pinned icon path, so a library filled in by an older
// extractor can be spotted and re-extracted.
// v4: shell icons padded into the corner of a jumbo tile are cropped (#42).
const QByteArray kCacheFormat = QByteArrayLiteral("v4");

// The shell's 256 px ("jumbo") image list does not scale an executable whose
// icon resource has no 256 px frame. It paints the largest frame it has, often
// 32 or 48 px, into the top-left corner of a transparent 256 px tile, and the
// library then showed a tiny glyph in an empty square (#42). Content confined
// to the top-left quadrant is that artifact: crop the smallest standard icon
// square that holds it, keeping the icon's own margin, and let the caller scale
// it. Anything else, including icons with deliberate padding, is left alone.
QImage cropShellIconPadding(const QImage& source)
{
    if (source.isNull())
        return source;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int width = image.width();
    const int height = image.height();
    int left = width;
    int top = -1;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < height; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(line[x]) != 0) {
                left = qMin(left, x);
                right = qMax(right, x);
                if (top < 0)
                    top = y;
                bottom = y;
            }
        }
    }
    if (right < 0 || right >= width / 2 || bottom >= height / 2)
        return source;
    const int extent = qMax(right, bottom) + 1;
    // The pasted frame starts at the tile's origin, so its artwork begins in
    // the first half of that frame. Art floating further in is a real design.
    if (left * 2 >= extent || top * 2 >= extent)
        return source;
    static const int kFrameSizes[] = { 16, 20, 24, 32, 40, 48, 64, 72, 96, 128 };
    int side = extent;
    for (const int size : kFrameSizes) {
        if (size >= extent) {
            side = size;
            break;
        }
    }
    side = qMin(side, qMin(width, height));
    return image.copy(0, 0, side, side);
}

} // namespace

QString GameIconCache::formatVersion()
{
    return QString::fromLatin1(kCacheFormat);
}

QStringList GameIconCache::manifestLogoReferencesForTesting(const QString& manifestPath)
{
    return manifestLogoReferences(manifestPath);
}

QString GameIconCache::resolveManifestAssetForTesting(const QString& manifestDir,
                                                       const QString& reference)
{
    return resolveManifestAsset(manifestDir, reference);
}

QImage GameIconCache::cropShellIconPaddingForTesting(const QImage& image)
{
    return cropShellIconPadding(image);
}

QString GameIconCache::iconPathForExecutable(const QString& executablePath)
{
    if (executablePath.isEmpty() || !QFileInfo::exists(executablePath)) {
        qWarning() << "GameIconCache: icon extraction skipped, executable path is empty or missing:"
                   << executablePath;
        return {};
    }

    QDir().mkpath(Paths::gameIconsDir());
    // The cache key carries the extractor's format version. A cache hit short
    // circuits everything below, so without it every executable already cached
    // under the old shell-icon-only extractor would keep serving that generic
    // .exe glyph and the package-manifest path would never run.
    const QByteArray hash = QCryptographicHash::hash(
        kCacheFormat + '\0' + executablePath.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString outPath = Paths::gameIconsDir() + QLatin1Char('/')
                            + QString::fromLatin1(hash) + QStringLiteral(".png");
    if (QFileInfo::exists(outPath))
        return outPath;

    QPixmap pixmap = packageLogoPixmap(executablePath);
    if (pixmap.isNull()) {
        QFileIconProvider provider;
        const QIcon icon = provider.icon(QFileInfo(executablePath));
        // Ask for the jumbo size so a padded tile is recognisable before any
        // downscale shrinks the real glyph to a few pixels.
        QImage image = icon.pixmap(QSize(256, 256), 1.0).toImage();
        image.setDevicePixelRatio(1.0);
        const QSize before = image.size();
        image = cropShellIconPadding(image);
        if (!image.isNull() && image.size() != before) {
            qInfo() << "GameIconCache: cropped a" << image.width() << "px icon out of a padded"
                    << before.width() << "px shell tile for" << executablePath;
        }
        if (!image.isNull() && image.width() < 64)
            image = image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap = QPixmap::fromImage(image);
    }
    if (pixmap.isNull()) {
        qWarning() << "GameIconCache: no icon resource found in executable:" << executablePath;
        return {};
    }
    if (!pixmap.save(outPath, "png")) {
        qWarning() << "GameIconCache: failed to write extracted icon to" << outPath
                   << "for executable" << executablePath;
        return {};
    }
    return outPath;
}
