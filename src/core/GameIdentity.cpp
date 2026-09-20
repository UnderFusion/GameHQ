#include "core/GameIdentity.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace
{
constexpr auto kFallbackName = "Unknown Game";
constexpr auto kForbiddenChars = "<>:\"/\\|?*";
}

namespace GameIdentity
{
QString folderName(const QString& name)
{
    const QString forbidden = QString::fromLatin1(kForbiddenChars);
    QString out;
    out.reserve(name.size());
    for (const QChar c : name)
        out += forbidden.contains(c) ? QLatin1Char('_') : c;
    out = out.trimmed();
    return out.isEmpty() ? QString::fromLatin1(kFallbackName) : out;
}

QString key(const QString& name)
{
    return folderName(name).simplified().toCaseFolded();
}

bool hasFolderForbiddenChar(const QString& name)
{
    const QString forbidden = QString::fromLatin1(kForbiddenChars);
    for (const QChar c : name) {
        if (forbidden.contains(c))
            return true;
    }
    return false;
}

QString executableKey(const QString& executablePathOrKey)
{
    const QString raw = executablePathOrKey.trimmed();
    if (raw.isEmpty())
        return QString();
    // canonicalFilePath() resolves separators, casing and links while the
    // executable is resolvable; a path that currently does not exist (an
    // uninstalled game) still normalizes deterministically. Both sides of a
    // match run through this, so the function only has to be idempotent.
    const QString canonical = QFileInfo(raw).canonicalFilePath();
    const QString key = canonical.isEmpty() ? QDir::cleanPath(raw) : canonical;
    return key.toLower();
}

QString inferFromPath(const QString& root, const QString& filePath)
{
    const QDir rootDir(root);
    const QString relative = rootDir.relativeFilePath(filePath);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    // "<Game>/Screenshots/file.png" or "<Game>/Clips/file.mp4"
    if (parts.size() >= 3)
        return parts.first();
    // "<Game>/file.png"
    if (parts.size() == 2)
        return parts.first();
    return QString::fromLatin1(kFallbackName);
}
}
