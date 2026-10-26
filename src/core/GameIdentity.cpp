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

QString pathComponent(const QString& name)
{
    QString out;
    const QString base = folderName(name);
    out.reserve(base.size());
    for (const QChar c : base)
        out += c.unicode() < 0x20 || c.unicode() == 0x7f ? QLatin1Char('_') : c;
    // Windows drops trailing dots and spaces, so "Game." and "Game" are one
    // folder; strip them here so the path we check is the one that is used.
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
        out.chop(1);
    out = out.trimmed();
    if (out.isEmpty())
        return QString::fromLatin1(kFallbackName);

    static const QStringList reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
    };
    const QString stem = out.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    bool device = reserved.contains(stem);
    if (!device && stem.size() == 4
        && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT")))) {
        const QChar digit = stem.at(3);
        // COM1-9 and LPT1-9, plus the superscript digits Windows also reserves.
        device = (digit >= QLatin1Char('1') && digit <= QLatin1Char('9'))
            || digit == QChar(0x00B9) || digit == QChar(0x00B2) || digit == QChar(0x00B3);
    }
    if (device) {
        // Windows reads the name before the first dot, so the '_' goes there.
        const qsizetype dot = out.indexOf(QLatin1Char('.'));
        if (dot < 0)
            out += QLatin1Char('_');
        else
            out.insert(dot, QLatin1Char('_'));
    }
    return out;
}

bool isInsideRoot(const QString& root, const QString& path)
{
    if (root.trimmed().isEmpty() || path.trimmed().isEmpty())
        return false;
    const QString cleanRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    const QString cleanPath = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString prefix = cleanRoot.endsWith(QLatin1Char('/')) ? cleanRoot
                                                                 : cleanRoot + QLatin1Char('/');
    return cleanPath.size() > prefix.size()
        && cleanPath.startsWith(prefix, Qt::CaseInsensitive);
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
