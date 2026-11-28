#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

class GameIconCache
{
public:
    static QString iconPathForExecutable(const QString& executablePath);

    // Bumped whenever the extractor learns to produce a better icon. Callers
    // that pinned an icon path persist this alongside it, so they can tell a
    // stale icon from a current one without re-reading every executable.
    static QString formatVersion();

    // Exposed for testing: the manifest-logo resolution steps, without going
    // through a real executable/shell icon lookup.
    static QStringList manifestLogoReferencesForTesting(const QString& manifestPath);
    static QString resolveManifestAssetForTesting(const QString& manifestDir,
                                                   const QString& reference);
    // Exposed for testing: the jumbo-tile padding crop applied to shell icons.
    static QImage cropShellIconPaddingForTesting(const QImage& image);
};
